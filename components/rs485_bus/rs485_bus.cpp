#include "rs485_bus.h"

#include <cstring>

namespace esphome::rs485_bus {

static const char *const TAG = "rs485_bus";

// SPSC ring index access (producer: ISR/burst, consumer: loop).
static inline uint32_t IRAM_ATTR ring_load_(volatile uint32_t *v) { return __atomic_load_n(v, __ATOMIC_ACQUIRE); }
static inline void IRAM_ATTR ring_store_(volatile uint32_t *v, uint32_t val) { __atomic_store_n(v, val, __ATOMIC_RELEASE); }

uint32_t IRAM_ATTR RS485Bus::now_us_() const {
#ifdef USE_ESP32
  return (uint32_t) esp_timer_get_time();
#else
  using namespace std::chrono;
  return (uint32_t) duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

bool RS485Bus::is_hw_isr() const {
#ifdef USE_ESP32
  return this->hw_ != nullptr;
#else
  return false;
#endif
}

void IRAM_ATTR RS485Bus::ring_push_(const BusRecord &rec) {
  uint32_t h = ring_load_(&this->ring_head_);
  uint32_t t = ring_load_(&this->ring_tail_);
  uint32_t next = (h + 1) & this->ring_mask_;
  if (next == t) {  // full: drop the newest trace entry; engine already saw the byte
    this->stats_.ring_overflows++;
    return;
  }
  this->ring_[h] = rec;
  ring_store_(&this->ring_head_, next);
}

void RS485Bus::dispatch_record_(const BusRecord &rec, uint32_t delta_us) {
  this->recent_[this->recent_idx_ % 64] = rec;
  this->recent_idx_++;
  for (auto *l : this->listeners_)
    l->on_bus_record(rec, delta_us);
}

void RS485Bus::debug_dump_recent() {
  char buf[64 * 5 + 1];
  size_t p = 0;
  uint32_t start = this->recent_idx_ >= 64 ? this->recent_idx_ - 48 : 0;
  uint32_t last_ts = 0;
  for (uint32_t i = start; i < this->recent_idx_; i++) {
    const BusRecord &rec = this->recent_[i % 64];
    uint32_t dt = last_ts ? rec.ts_us - last_ts : 0;
    last_ts = rec.ts_us;
    if (rec.dir == DIR_TX)  // our own transmissions, visually distinct from bus bytes
      p += snprintf(buf + p, sizeof(buf) - p, "[%02X]%s", rec.byte, dt > 2000 ? "|" : " ");
    else if (rec.dir & DIR_HB9)  // ninth bit read 1 -> ^ after the byte
      p += snprintf(buf + p, sizeof(buf) - p, "%02X^%s", rec.byte, dt > 2000 ? "|" : " ");
    else
      p += snprintf(buf + p, sizeof(buf) - p, "%02X%s", rec.byte, dt > 2000 ? "|" : " ");
  }
  buf[p] = 0;
  ESP_LOGW(TAG, "recent wire (bytes, | = >2ms gap): %s", buf);
}

// ------------------------- hardware mode: ESP32-S3 raw UART ISR -------------------------
#ifdef USE_ESP32

void IRAM_ATTR RS485Bus::en_set_level_(bool transmit) {
  if (this->en_w1_reg_) {
    REG_WRITE(transmit ? GPIO_OUT1_W1TS_REG : GPIO_OUT1_W1TC_REG, this->en_gpio_mask_);
  } else {
    REG_WRITE(transmit ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, this->en_gpio_mask_);
  }
}

void IRAM_ATTR RS485Bus::uart_isr_(void *arg) {
  auto *self = static_cast<RS485Bus *>(arg);
  uart_dev_t *hw = self->hw_;
  while (true) {
    uint32_t pend = uart_ll_get_intsts_mask(hw) & uart_ll_get_intr_ena_status(hw);
    if (pend == 0)
      break;
    uint32_t ts = (uint32_t) esp_timer_get_time();

    if (pend & (UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT)) {
      uint32_t n = uart_ll_get_rxfifo_len(hw);
      for (uint32_t i = 0; i < n; i++) {
        uint32_t bts = (uint32_t) esp_timer_get_time();
        uint8_t b;
        // parity flag leads its byte out of the FIFO by a half bit-time: read-consume it per byte
        uint32_t par = hw->int_raw.val & UART_INTR_PARITY_ERR;
        uart_ll_read_rxfifo(hw, &b, 1);
        if (par)
          uart_ll_clr_intsts_mask(hw, UART_INTR_PARITY_ERR);
        uint32_t d = bts - self->last_rx_ts_;
        if (self->stats_.rx_bytes > 0 && d < self->stats_.min_rx_delta_us)
          self->stats_.min_rx_delta_us = d;
        // even-parity slot read: hidden bit = parity-error XOR computed parity of the data byte
        uint8_t hb = (par != 0) ^ (__builtin_parity(b) != 0);
        if (par)
          self->stats_.par_errs++;
        uint8_t flags = hb ? RXF_HB9 : 0;
        self->stats_.rx_bytes++;
        self->last_rx_ts_ = bts;
        self->gap_fired_ = false;
        self->ring_push_(BusRecord{bts, b, (uint8_t) (DIR_RX | (hb ? DIR_HB9 : 0))});
        if (self->hooks_.on_rx_byte != nullptr)
          self->hooks_.on_rx_byte(self->engine_, b, bts, flags);
      }
      // Surface at most one gap event per RX burst (some chips re-fire tout while idle).
      if ((pend & UART_INTR_RXFIFO_TOUT) != 0 && !self->gap_fired_) {
        self->gap_fired_ = true;
        self->stats_.gaps++;
        if (self->hooks_.on_gap != nullptr)
          self->hooks_.on_gap(self->engine_, ts);
      }
      uart_ll_clr_intsts_mask(hw, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT);
    }

    if (pend & UART_INTR_TX_DONE) {
      // same instant the IDF RS485 half-duplex driver drops DE; receiver hw-muted during TX — no echo
      uart_ll_disable_intr_mask(hw, UART_INTR_TX_DONE);
      uart_ll_clr_intsts_mask(hw, UART_INTR_TX_DONE);
      self->en_set_level_(false);
      __atomic_store_n(&self->tx_busy_, 0u, __ATOMIC_RELEASE);
      if (self->hooks_.on_tx_done != nullptr)
        self->hooks_.on_tx_done(self->engine_, ts);
    }

    uint32_t errs = pend & (UART_INTR_FRAM_ERR | UART_INTR_GLITCH_DET);
    if (errs) {
      // frm: the stop slot sampled 0 — the 9-bit-with-space-data candidate tell (docs/hypotheses.md)
      if (errs & UART_INTR_FRAM_ERR)
        self->stats_.frm_errs++;
      if (errs & UART_INTR_GLITCH_DET)
        self->stats_.glitches++;
      uart_ll_clr_intsts_mask(hw, errs);
    }
    if (pend & UART_INTR_PARITY_ERR) {
      uart_ll_clr_intsts_mask(hw, UART_INTR_PARITY_ERR);  // stray flag with its byte already read
    }
    if (pend & UART_INTR_RXFIFO_OVF) {
      self->stats_.rx_hw_overflows++;
      uart_ll_clr_intsts_mask(hw, UART_INTR_RXFIFO_OVF);
    }
  }
}

void RS485Bus::hw_setup_() {
#if !defined(USE_ESP32_VARIANT_ESP32S3)
#error "rs485_bus hardware mode currently supports ESP32-S3 only (the Waveshare board used here)"
#endif
  if (this->rx_pin_ == nullptr || this->tx_pin_ == nullptr || this->en_pin_ == nullptr) {
    ESP_LOGE(TAG, "hardware mode requires rx_pin, tx_pin and en_pin");
    return;
  }
  if (!this->rx_pin_->is_internal() || !this->tx_pin_->is_internal() || !this->en_pin_->is_internal()) {
    ESP_LOGE(TAG, "rx_pin, tx_pin and en_pin must be internal GPIOs");
    return;
  }

  this->rx_pin_->setup();
  this->tx_pin_->setup();
  this->en_pin_->setup();
  this->en_pin_->digital_write(false);  // hold the transceiver in receive mode

  auto *en_internal = reinterpret_cast<esphome::InternalGPIOPin *>(this->en_pin_);
  uint8_t en_num = en_internal->get_pin();
  this->en_gpio_mask_ = (en_num < 32) ? (1u << en_num) : (1u << (en_num - 32));
  this->en_w1_reg_ = en_num >= 32;

  // claim the UART peripheral — no IDF uart driver anywhere in the chain
  uart_dev_t *hw = UART_LL_GET_HW(this->uart_num_);
  this->hw_ = hw;
  // direct peripheral-domain control: the UART clock-enable/reset-domain bits written directly —
  // LL wrappers did nothing, periph_module_enable crashed the boot
  uint32_t clk_reg, rst_reg, clk_mask_ = 0, rst_mask_ = 0;
  switch (this->uart_num_) {
    case 0: clk_reg = SYSTEM_PERIP_CLK_EN0_REG; rst_reg = SYSTEM_PERIP_RST_EN0_REG;
            clk_mask_ = SYSTEM_UART_CLK_EN; rst_mask_ = SYSTEM_UART_RST; break;
    case 1: clk_reg = SYSTEM_PERIP_CLK_EN0_REG; rst_reg = SYSTEM_PERIP_RST_EN0_REG;
            clk_mask_ = SYSTEM_UART1_CLK_EN; rst_mask_ = SYSTEM_UART1_RST; break;
    default: clk_reg = SYSTEM_PERIP_CLK_EN1_REG; rst_reg = SYSTEM_PERIP_RST_EN1_REG;
             clk_mask_ = SYSTEM_UART2_CLK_EN; rst_mask_ = SYSTEM_UART2_RST; break;
  }
  this->sys_clk_mask_ = clk_mask_;
  this->sys_rst_mask_ = rst_mask_;
  this->sys_use_en1_ = this->uart_num_ == 2;
  SET_PERI_REG_MASK(clk_reg, clk_mask_);
  CLEAR_PERI_REG_MASK(rst_reg, rst_mask_);
  bool clk_now = (REG_READ(clk_reg) & clk_mask_) != 0;
  bool rst_now = (REG_READ(rst_reg) & rst_mask_) == 0;
  ESP_LOGI(TAG, "hw-init v9: uart%d clk=%d rst_free=%d en1=%d",
           this->uart_num_, clk_now, rst_now, (int) this->sys_use_en1_);
  if (!clk_now || !rst_now) {
    ESP_LOGE(TAG, "UART%d peripheral domain failed to enable", this->uart_num_);
    return;
  }
  uart_ll_sclk_enable(hw);
  uart_ll_set_sclk(hw, (soc_module_clk_t) UART_SCLK_APB);  // 80 MHz on S3
  uart_ll_set_baudrate(hw, this->baud_, 80000000);
  uart_ll_set_data_bit_num(hw, UART_DATA_8_BITS);
  uart_ll_set_parity(hw, UART_PARITY_EVEN);  // the 9th marking slot: RX samples it, TX emits it (hypotheses.md 15)
  uart_ll_set_stop_bits(hw, UART_STOP_BITS_1);
  uart_ll_set_mode_rs485_app_ctrl(hw);  // receiver muted while transmitting: no echo

  uart_ll_rxfifo_rst(hw);
  uart_ll_txfifo_rst(hw);
  uart_ll_set_rxfifo_full_thr(hw, 1);  // per-byte RX interrupts
  uart_ll_set_rx_tout(hw, (uint16_t) this->gap_symbols_);
  hw->conf1.rx_tout_en = 1;
  uart_ll_clr_intsts_mask(hw, UART_LL_INTR_MASK);

  // IO-MUX function + input-enable set explicitly (the ROM matrix helper skips the pad side — no FUN_IE = silent pin)
  int tx = reinterpret_cast<esphome::InternalGPIOPin *>(this->tx_pin_)->get_pin();
  int rx = reinterpret_cast<esphome::InternalGPIOPin *>(this->rx_pin_)->get_pin();
  gpio_ll_func_sel(&GPIO, (uint8_t) rx, PIN_FUNC_GPIO);
  gpio_ll_func_sel(&GPIO, (uint8_t) tx, PIN_FUNC_GPIO);
  gpio_ll_input_enable(&GPIO, (uint32_t) rx);
  switch (this->uart_num_) {
    case 0:
      esp_rom_gpio_connect_out_signal(tx, U0TXD_OUT_IDX, false, false);
      esp_rom_gpio_connect_in_signal(rx, U0RXD_IN_IDX, false);
      break;
    case 2:
      esp_rom_gpio_connect_out_signal(tx, U2TXD_OUT_IDX, false, false);
      esp_rom_gpio_connect_in_signal(rx, U2RXD_IN_IDX, false);
      break;
    default:
      esp_rom_gpio_connect_out_signal(tx, U1TXD_OUT_IDX, false, false);
      esp_rom_gpio_connect_in_signal(rx, U1RXD_IN_IDX, false);
      break;
  }

  int source = (this->uart_num_ == 0) ? ETS_UART0_INTR_SOURCE
              : ((this->uart_num_ == 1) ? ETS_UART1_INTR_SOURCE : ETS_UART2_INTR_SOURCE);
  esp_err_t err = esp_intr_alloc(source, ESP_INTR_FLAG_IRAM, &RS485Bus::uart_isr_, this, &this->intr_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_intr_alloc failed: %d — bus stays passive", err);
    return;
  }

  this->active_ = this->want_active_;
  // RX interrupts on; TX_DONE is armed per burst in send_burst
  uart_ll_ena_intr_mask(hw, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT | UART_INTR_RXFIFO_OVF |
                                 UART_INTR_FRAM_ERR | UART_INTR_PARITY_ERR | UART_INTR_GLITCH_DET);
  ESP_LOGCONFIG(TAG, "hardware (ISR) mode: uart%d, %u baud %s, gap=%u byte-times, active=%d", this->uart_num_,
                (unsigned) this->baud_, "9-bit",
                (unsigned) this->gap_symbols_, this->active_);
  // immediate post-setup readback: catches "configured but dead domain" instantly
  ESP_LOGI(TAG, "hw-init v8 readback: id=%04x conf0=%08x conf1=%08x int_ena=%08x status=%08x",
           (unsigned) hw->id.val, (unsigned) hw->conf0.val, (unsigned) hw->conf1.val,
           (unsigned) hw->int_ena.val, (unsigned) hw->status.val);
}
#endif  // USE_ESP32

bool IRAM_ATTR RS485Bus::send_burst(const uint8_t *data, size_t len, bool marked) {
  if (len == 0 || len > MAX_BURST) {
    this->stats_.tx_refused++;
    return false;
  }
#ifdef USE_ESP32
  if (this->hw_ == nullptr) {
    // fall through to stream mode below when a uart_ is configured
#  ifdef USE_UART
    if (this->uart_ == nullptr) {
      this->stats_.tx_refused++;
      return false;
    }
#  else
    this->stats_.tx_refused++;
    return false;
#  endif
  } else {
    if (!this->active_) {
      this->stats_.tx_refused++;
      return false;
    }
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&this->tx_busy_, &expected, 1u, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
      this->stats_.tx_refused++;
      return false;
    }

    this->en_set_level_(true);
    uint32_t byte_us = 11u * (1000000u / this->baud_);  // char time incl. marking slot + stop
    uint32_t ts0 = this->now_us_();
    // Clear any stale tx-done before arming it (the pitfall the IDF rs485 driver guards against).
    uart_ll_clr_intsts_mask(this->hw_, UART_INTR_TX_DONE);
    // one byte at a time: conf0.parity must only change while the TX FSM is idle (pop/shift sampling stays safe)
    for (size_t i = 0; i < len; i++) {
      if (i > 0) {  // bounded wait for the previous char to fully leave the wire (multi-byte bursts only)
        for (uint32_t g = 0; g < 300000u && !uart_ll_is_tx_idle(this->hw_); g++) {
        }
      }
      // conf0.parity: 0=EVEN, 1=ODD; pick the mode whose generated parity bit equals the wanted 9th slot value
      this->hw_->conf0.parity = (__builtin_parity(data[i]) != (marked ? 1 : 0));
      uart_ll_write_txfifo(this->hw_, &data[i], 1);
    }
    uart_ll_ena_intr_mask(this->hw_, UART_INTR_TX_DONE);

    for (size_t i = 0; i < len; i++)
      this->ring_push_(BusRecord{ts0 + i * byte_us, data[i], DIR_TX});

    this->stats_.tx_bursts++;
    this->stats_.tx_bytes += len;
    return true;
  }
#endif
#ifdef USE_RS485_BUS_STREAM
  if (!this->active_ || this->uart_ == nullptr) {
    this->stats_.tx_refused++;
    return false;
  }
  uint32_t ts0 = this->now_us_();
  uint32_t byte_us = 10000000u / this->baud_;
  this->uart_->write_array(data, len);  // marked: no parity slot exists on a stream; host sends 8N1 bytes
  this->uart_->flush();
  for (size_t i = 0; i < len; i++)
    this->ring_push_(BusRecord{ts0 + i * byte_us, data[i], DIR_TX});
  this->stats_.tx_bursts++;
  this->stats_.tx_bytes += len;
  if (this->hooks_.on_tx_done != nullptr)
    this->hooks_.on_tx_done(this->engine_, this->now_us_());
  return true;
#else
  this->stats_.tx_refused++;
  return false;
#endif
}

// ------------------------------ stream mode (host builds) ------------------------------
void RS485Bus::stream_setup_() {
#ifdef USE_RS485_BUS_STREAM
  if (this->uart_ == nullptr) {
    ESP_LOGE(TAG, "stream mode requires uart_id");
    return;
  }
  this->active_ = this->want_active_;
  ESP_LOGCONFIG(TAG, "stream mode: esphome uart, active=%d", this->active_);
#endif
}

void RS485Bus::setup() {
  // trace ring
  size_t cap = this->ring_cap_;
  if (cap < 512)
    cap = 512;
  size_t pow2 = 1;
  while (pow2 < cap)
    pow2 <<= 1;
  this->ring_mask_ = pow2 - 1;
  this->ring_ = new BusRecord[pow2];

  this->gap_us_ = this->gap_symbols_ * (10000000u / this->baud_);
  this->stats_.min_rx_delta_us = ~0u;  // min still unseen

#ifdef USE_ESP32
  if (!this->is_stream())
    this->hw_setup_();
  else
    this->stream_setup_();
#else
  this->stream_setup_();
  if (this->uart_ == nullptr)
    ESP_LOGE(TAG, "host build requires uart_id (tcp_uart)");
#endif
}

void RS485Bus::loop() {
#ifdef USE_ESP32
  // RX-starvation watch: while no byte ever arrived, surface hw state and re-arm zeroed RX interrupt enables
  static uint32_t diag_ms = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  if (this->hw_ != nullptr && this->stats_.rx_bytes == 0) {
    uint32_t now_ms = esphome::millis();
    if (now_ms - diag_ms > 10000) {
      diag_ms = now_ms;
      uint32_t clk_reg2 = this->sys_use_en1_ ? SYSTEM_PERIP_CLK_EN1_REG : SYSTEM_PERIP_CLK_EN0_REG;
      uint32_t rst_reg2 = this->sys_use_en1_ ? SYSTEM_PERIP_RST_EN1_REG : SYSTEM_PERIP_RST_EN0_REG;
      ESP_LOGI(TAG, "rx diag: id=%04x enabled=%d fifo=%u status=%08x int_raw=%08x int_ena=%08x conf0=%08x conf1=%08x clk_en=%08x rst_en=%08x",
               (unsigned) this->hw_->id.val, uart_ll_is_enabled((uint32_t) this->uart_num_),
               (unsigned) hw_->status.rxfifo_cnt, (unsigned) this->hw_->status.val,
               (unsigned) this->hw_->int_raw.val, (unsigned) this->hw_->int_ena.val,
               (unsigned) this->hw_->conf0.val, (unsigned) this->hw_->conf1.val,
               (unsigned) REG_READ(clk_reg2), (unsigned) REG_READ(rst_reg2));
      if ((this->hw_->int_ena.val &
           (UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT)) == 0) {
        ESP_LOGW(TAG, "rx interrupts disabled post-setup — re-arming");
        uart_ll_ena_intr_mask(this->hw_, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT |
                                           UART_INTR_RXFIFO_OVF | UART_INTR_FRAM_ERR |
                                           UART_INTR_PARITY_ERR | UART_INTR_GLITCH_DET);
      }
      // self-heal: whatever disabled the domain, the sniffer keeps re-enabling it
      uint32_t clk_reg3 = this->sys_use_en1_ ? SYSTEM_PERIP_CLK_EN1_REG : SYSTEM_PERIP_CLK_EN0_REG;
      uint32_t rst_reg3 = this->sys_use_en1_ ? SYSTEM_PERIP_RST_EN1_REG : SYSTEM_PERIP_RST_EN0_REG;
      if ((REG_READ(clk_reg3) & this->sys_clk_mask_) == 0 ||
          (REG_READ(rst_reg3) & this->sys_rst_mask_) != 0) {
        ESP_LOGW(TAG, "uart%d domain was off — re-enabling", this->uart_num_);
        SET_PERI_REG_MASK(clk_reg3, this->sys_clk_mask_);
        CLEAR_PERI_REG_MASK(rst_reg3, this->sys_rst_mask_);
      }
    }
  }
#endif
#ifdef USE_RS485_BUS_STREAM
  if (this->is_stream()) {
    // stream mode: feed the engine byte-by-byte and derive gap events
    uint8_t b;
    while (this->uart_->available()) {
      uint32_t bts = this->now_us_();
      this->uart_->read_byte(&b);
      this->stats_.rx_bytes++;
      this->last_rx_ts_ = bts;
      this->gap_fired_ = false;
      this->ring_push_(BusRecord{bts, b, DIR_RX});
      if (this->hooks_.on_rx_byte != nullptr)
        this->hooks_.on_rx_byte(this->engine_, b, bts, 0);  // flags: parity slot is a hardware-mode probe
    }
    uint32_t now = this->now_us_();
    if (!this->gap_fired_ && (now - this->last_rx_ts_) >= this->gap_us_) {
      this->gap_fired_ = true;
      this->stats_.gaps++;
      if (this->hooks_.on_gap != nullptr)
        this->hooks_.on_gap(this->engine_, now);
    }
  }
#endif

  // drain the trace ring to listeners
  uint32_t h = ring_load_(&this->ring_head_);
  uint32_t t = ring_load_(&this->ring_tail_);
  uint32_t prev_ts = 0;
  bool have_prev = false;
  while (t != h) {
    const BusRecord &rec = this->ring_[t];
    uint32_t delta = have_prev ? rec.ts_us - prev_ts : 0;
    this->dispatch_record_(rec, delta);
    prev_ts = rec.ts_us;
    have_prev = true;
    t = (t + 1) & this->ring_mask_;
  }
  if (t != ring_load_(&this->ring_tail_))
    ring_store_(&this->ring_tail_, t);
}

void RS485Bus::dump_config() {
  ESP_LOGCONFIG(TAG, "RS485Bus:");
  ESP_LOGCONFIG(TAG, "  mode: %s", this->is_hw_isr() ? "hardware ISR" : "stream");
  ESP_LOGCONFIG(TAG, "  baud: %u, gap: %u byte-times (~%u us)", (unsigned) this->baud_,
                (unsigned) this->gap_symbols_, (unsigned) this->gap_us_);
  ESP_LOGCONFIG(TAG, "  active: %d", this->active_);
  ESP_LOGCONFIG(TAG, "  line: 9-bit, marking slot on TX+RX (engine passes per-byte marks; docs/hypotheses.md 15)");
  ESP_LOGCONFIG(TAG, "  engine attached: %s", this->engine_ != nullptr ? "yes" : "no");
}

}  // namespace esphome::rs485_bus
