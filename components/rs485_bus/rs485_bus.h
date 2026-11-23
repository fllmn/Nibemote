#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_private/periph_ctrl.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "hal/uart_types.h"
#include "hal/uart_ll.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/interrupts.h"
#include "soc/io_mux_reg.h"
#include "soc/uart_struct.h"
#endif

#ifdef USE_RS485_BUS_STREAM
#include "esphome/components/uart/uart_component.h"
#endif

namespace esphome::rs485_bus {

static constexpr size_t MAX_BURST = 96;  // one EN-high burst (hw fifo is 128; keep headroom)

enum RecordDir : uint8_t { DIR_RX = 0, DIR_TX = 1 };
static constexpr uint8_t DIR_HB9 = 0x80;    // RX record: hidden parity-slot bit reads 1 (probe mode only)
static constexpr uint8_t RXF_HB9 = 0x80;     // on_rx_byte flags: the byte's parity slot carried 1

/// One byte on the wire as seen by this device.
struct BusRecord {
  uint32_t ts_us;  // µs timestamp at byte event (wraps ~71.6 min; deltas are what matter)
  uint8_t byte;
  uint8_t dir;  // RecordDir (+DIR_HB9 on RX records in probe mode)
} __attribute__((packed));

/// Task-context consumer of the raw byte stream (called from loop(), never ISR).
class BusRecordListener {
 public:
  virtual ~BusRecordListener() = default;
  virtual void on_bus_record(const BusRecord &rec, uint32_t delta_us) = 0;
};

/// Engine hooks: raw UART ISR context on hardware — ISR-safe: IRAM code, DRAM data, no locks/log/virtual.
/// Stream mode calls them from loop(); the engine cannot tell which context it is in.
struct EngineHooks {
  // flags: per-byte RXF_* bits — RXF_HB9 set when the byte's parity slot sampled 1 (probe mode; 0 otherwise)
  void (*on_rx_byte)(void *ctx, uint8_t byte, uint32_t ts_us, uint8_t flags) = nullptr;
  void (*on_gap)(void *ctx, uint32_t ts_us) = nullptr;
  void (*on_tx_done)(void *ctx, uint32_t ts_us) = nullptr;
};

struct BusStats {
  uint32_t rx_bytes, tx_bytes;
  uint32_t rx_hw_overflows;  // UART hw FIFO overran (bytes lost — engine recovers on next gap)
  uint32_t frm_errs;         // stop-bit slot sampled 0 (per-byte, does not cascade; see docs/hypotheses.md)
  uint32_t par_errs;         // parity-slot mismatch (probe mode: the hidden 9th bit differs from even parity)
  uint32_t glitches;         // glitch-detector ints (line noise)
  uint32_t hb1, hb0;          // probe census: bytes whose parity slot sampled 1 vs 0
  uint32_t min_rx_delta_us;  // smallest byte-to-byte RX delta seen (~char width: 8N1=~521, 9N1=~573 @19200)
  uint32_t ring_overflows;   // trace ring full (log data dropped, engine unaffected)
  uint32_t tx_bursts, tx_refused;
  uint32_t gaps;
};

/// Generic ISR-driven half-duplex RS485 transport (DE/~RE tied line); protocol-blind — the engine sees
/// bytes, gaps and its own TX completions.
///
/// Hardware (pins given): UART owned by a raw IRAM ISR, per-byte RX; EN dropped on the exact UART_TX_DONE
/// (IDF half-duplex timing; receiver muted during TX — no local echo).
/// Stream (uart id given): esphome `uart` (host tcp_uart / task builds); gaps from arrival times, EN unused.
class RS485Bus final : public Component {
 public:
  RS485Bus() {}
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

  void set_rx_pin(GPIOPin *pin) { this->rx_pin_ = pin; }
  void set_tx_pin(GPIOPin *pin) { this->tx_pin_ = pin; }
  void set_en_pin(GPIOPin *pin) { this->en_pin_ = pin; }
  void set_uart_num(int n) { this->uart_num_ = n; }
  void set_baud_rate(uint32_t b) { this->baud_ = b; }
  void set_gap_symbols(uint32_t s) { this->gap_symbols_ = s; }
  void set_active(bool a) { this->want_active_ = a; }
  void set_rx_buffer(size_t entries) { this->ring_cap_ = entries; }

#ifdef USE_RS485_BUS_STREAM
  void set_uart(uart::UARTComponent *uart) { this->uart_ = uart; }
  bool is_stream() const { return this->uart_ != nullptr; }
#else
  bool is_stream() const { return false; }
#endif

  void attach_engine(void *ctx, EngineHooks hooks) {
    this->engine_ = ctx;
    this->hooks_ = hooks;
  }

  /// True when this bus is allowed to drive the line (config `active: true`).
  bool bus_active() const { return this->active_; }
  bool is_hw_isr() const;
  uint32_t get_baud_rate() const { return this->baud_; }

  /// Send one EN-high burst (ISR or loop); refuses when passive, busy or oversized — idle policy is the engine's.
  /// `marked`: every byte's 9th slot carries 1 (address/token) instead of 0 (data).
  /// len>1 waits ~one char-time per byte between parity flips (loop context only; ISR callers send one byte).
  bool IRAM_ATTR send_burst(const uint8_t *data, size_t len, bool marked = false);  // callable from ISR

  void register_record_listener(BusRecordListener *l) { this->listeners_.push_back(l); }
  const BusStats &stats() const { return this->stats_; }

  /// hexdump the recent bus bytes (task context) — the engine calls this on anomalies to keep the wire context.
  void debug_dump_recent();

  /// µs since the last RX byte (age of the last bus activity; ISR-safe source).
  uint32_t rx_silence_us() const { return this->now_us_() - this->last_rx_ts_; }

  /// the bus's µs clock (esp_timer / steady_clock) — the domain that stamps RX; engines time replies against it.
  uint32_t IRAM_ATTR now_us() const { return this->now_us_(); }

 protected:
  uint32_t IRAM_ATTR now_us_() const;
  void IRAM_ATTR ring_push_(const BusRecord &rec);
  void dispatch_record_(const BusRecord &rec, uint32_t delta_us);

#ifdef USE_ESP32
  void hw_setup_();
  static void IRAM_ATTR uart_isr_(void *arg);
  void IRAM_ATTR en_set_level_(bool transmit);
#endif
  void stream_setup_();

  GPIOPin *rx_pin_{nullptr};
  GPIOPin *tx_pin_{nullptr};
  GPIOPin *en_pin_{nullptr};
  int uart_num_{1};
  uint32_t baud_{19200};
  uint32_t gap_symbols_{4};  // rx-timeout threshold in byte-time units (≈ this bus's inter-frame gap)
  bool want_active_{false};
  bool active_{false};
  size_t ring_cap_{8192};

#ifdef USE_RS485_BUS_STREAM
  uart::UARTComponent *uart_{nullptr};
#endif

  void *engine_{nullptr};
  EngineHooks hooks_{};

  // raw-byte trace ring (SPSC; producer: ISR / send_burst, consumer: loop)
  BusRecord *ring_{nullptr};
  uint32_t ring_mask_{0};
  BusRecord recent_[64];  // rolling task-side copy for anomaly dumps
  uint32_t recent_idx_{0};
  volatile uint32_t ring_head_{0};
  volatile uint32_t ring_tail_{0};

  BusStats stats_{};

  volatile uint32_t last_rx_ts_{0};
  uint32_t gap_us_{2000};  // derived from baud and gap_symbols_
  bool gap_fired_{true};   // true until the next RX byte after each gap

  std::vector<BusRecordListener *> listeners_;

#ifdef USE_ESP32
  uart_dev_t *hw_{nullptr};
  intr_handle_t intr_handle_{nullptr};
  uint32_t sys_clk_mask_{0};  // clk-en bit for our UART (EN0 or EN1 file)
  uint32_t sys_rst_mask_{0};  // rst-en bit for our UART (EN0 or EN1 file)
  bool sys_use_en1_{false};   // UART2 lives in the *EN1 register file on S3
  uint32_t en_gpio_mask_{0};
  bool en_w1_reg_{false};        // pin >= 32 → GPIO_OUT1_*
  volatile uint32_t tx_busy_{0};  // CAS flag: exactly one burst on the wire
#endif
};

}  // namespace esphome::rs485_bus
