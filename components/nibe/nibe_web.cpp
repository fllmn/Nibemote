// Virtual display page (ported from ../nibemote): HTTP responder + button bank for the live display state.
//
// One connection at a time, one self-contained page, 2 s auto-refresh; enabled via display_web_port.
//
// Served from loop(): page building can delay event draining only, never a wire reply (ISR-driven timing).
#include "nibe_bus.h"

#ifdef USE_NIBE_DISPLAY_WEB

#include <cerrno>

namespace esphome {
namespace nibe {

static const char *const TAG = "nibe";

// page keys = the panel's cmd-40 key ids; knob detent = 12 placements, 10 detents a side of 127 (clamped)
static constexpr uint8_t WEB_KNOB_STEP = 12;
static const uint8_t WEB_KEY_PLUS = 0x01;
static const uint8_t WEB_KEY_MINUS = 0x02;
static const uint8_t WEB_KEY_ENTER = 0x04;

// operating bits of the symbol strip (row 1 / cmd 50; see docs/commands.md)
struct WebSymbolBit {
  uint8_t byte;
  uint8_t bit;
  const char *label;
  bool confirmed;
};
static const WebSymbolBit WEB_BITS[] = {
    {0, 7, "compressor symbol", false},
    {0, 6, "compressor A", false},
    {0, 5, "compressor B", true},
    {0, 4, "?", false},
    {0, 3, "?", false},
    {0, 2, "?", false},
    {0, 1, "?", false},
    {0, 0, "?", false},
    {1, 7, "extra hot water", true},
    {1, 6, "hw A (3h)", true},
    {1, 5, "hw B (periodic)", false},
    {1, 4, "?", false},
    {1, 3, "?", false},
    {1, 2, "?", false},
    {1, 1, "?", false},
    {1, 0, "?", false},
    {2, 7, "pump", true},
    {2, 6, "?", false},
    {2, 5, "?", false},
    {2, 4, "?", false},
    {2, 3, "heating system", true},
    {2, 2, "? (defrost?)", false},
    {2, 1, "? (addition heater?)", false},
    {2, 0, "? (addition heater?)", false},
};

static size_t web_append_escaped(std::string &out, const char *text) {
  for (const char *p = text; *p; p++) {
    switch (*p) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out += *p;
    }
  }
  return out.size();
}

// Minimal JSON string escaper (rows are already UTF-8).
static void json_str_(std::string &out, const char *text) {
  out += '"';
  for (const char *p = text; *p; p++) {
    unsigned char c = (unsigned char) *p;
    if (c == '"' || c == '\\') {
      out += '\\';
      out += (char) c;
    } else if (c < 0x20) {
      continue;  // control bytes: dropped
    } else {
      out += (char) c;
    }
  }
  out += '"';
}

// Builds the JSON state snapshot served at /j (polled by the page script).
std::string NibeBus::build_display_json_() {
  std::string row2 = this->snap_rows_[0];
  std::string row3 = this->snap_rows_[1];
  std::string row4 = this->snap_rows_[2];
  auto trim = [](const std::string &s) -> std::string {
    size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos)
      return "";
    size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
  };
  row2 = trim(row2);
  row3 = trim(row3);
  std::string r4l = row4, r4r;
  size_t ws = row4.find("  ");
  if (ws != std::string::npos) {
    r4l = row4.substr(0, ws);
    size_t end = row4.find_last_not_of(' ');
    size_t start = row4.find_first_not_of(' ', ws);
    if (start != std::string::npos && end != std::string::npos && end >= start)
      r4r = row4.substr(start, end - start + 1);
  }

  std::string out;
  out.reserve(320);
  out += "{\"v\":";
  json_str_(out, row2.c_str());
  out += ",\"l\":";
  json_str_(out, row3.c_str());
  out += ",\"m\":";
  json_str_(out, r4l.c_str());
  out += ",\"c\":";
  json_str_(out, r4r.c_str());
  out += ",\"hx\":\"";
  char hex[8];
  for (uint8_t i = 0; i < this->sym_raw_len_; i++) {
    snprintf(hex, sizeof(hex), "%s%02X", i ? " " : "", (unsigned) this->sym_raw_[i]);
    out += hex;
  }
  out += "\"";
  char buf[16];
  snprintf(buf, sizeof(buf), ",\"k\":%u}", (unsigned) this->knob_pos_);
  out += buf;
  return out;
}

std::string NibeBus::build_display_page_() {
  // the master pads the rows with spaces; trim for rendering
  auto trim = [](const std::string &s) -> std::string {
    size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos)
      return "";
    size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
  };
  std::string row2 = trim(this->snap_rows_[0]);
  std::string row3 = trim(this->snap_rows_[1]);
  std::string row4 = this->snap_rows_[2];

  // symbol strip state (see WEB_BITS for the mapping)
  auto bit_on = [this](uint8_t byte, uint8_t mask) -> bool {
    return this->sym_raw_len_ > byte && (this->sym_raw_[byte] & mask);
  };
  bool comp_on = bit_on(0, 0x80);
  bool comp_a = bit_on(0, 0x40);
  bool comp_b = bit_on(0, 0x20);
  bool hw_on = bit_on(1, 0x80);
  bool hw_a = bit_on(1, 0x40);
  bool hw_b = bit_on(1, 0x20);
  bool pump_on = bit_on(2, 0x80);
  bool heat_on = bit_on(2, 0x08);
  // addition heater I/II/III and defrost bits are not mapped yet — always dim

  // row 4: split "menu        clock" into left/right values
  std::string r4l, r4r;
  size_t ws = row4.find("  ");
  if (ws == std::string::npos) {
    r4l = row4;
  } else {
    r4l = row4.substr(0, ws);
    size_t end = row4.find_last_not_of(' ');
    size_t start = row4.find_first_not_of(' ', ws);
    if (start != std::string::npos && end != std::string::npos && end >= start)
      r4r = row4.substr(start, end - start + 1);
  }

  std::string page;
  page.reserve(5400);
  page +=
      "<!doctype html><html><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>VVM300</title><style>"
      "body{background:#191919;color:#bbb;font-family:sans-serif;display:flex;"
      "flex-direction:column;align-items:center;padding-top:24px;margin:0}"
      ".lcd{background:#04140a;border:16px solid #333;border-radius:20px;"
      "padding:20px 28px 24px;width:40ch;box-shadow:0 0 24px #000}"
      ".lcd *{font-family:monospace}"
      ".syms{display:grid;grid-template-columns:repeat(6,1fr);font-size:26px;"
      "margin-bottom:12px;text-shadow:0 0 10px #3aff5e66}"
      ".syms>div{display:flex;flex-direction:column;align-items:center}"
      ".ann{font-size:15px;font-weight:bold;min-height:18px;margin-bottom:3px;"
      "letter-spacing:2px;white-space:nowrap}"
      ".ann .lit{color:#ffdf3a}.ann .dim{color:#153a15}"
      ".on{color:#3aff5e}.off{opacity:.16;filter:grayscale(1)}"
      ".row1{font:bold 52px monospace;color:#3aff5e;text-align:center;"
      "margin:12px 0;min-height:68px;text-shadow:0 0 16px #3aff5e55;white-space:pre}"
      ".row2{font:bold 30px monospace;color:#3aff5e;text-align:left;"
      "margin:4px 0;min-height:40px;text-shadow:0 0 12px #3aff5e44;white-space:pre}"
      ".row3{font:bold 30px monospace;color:#3aff5e;display:flex;"
      "justify-content:space-between;margin:4px 0;min-height:40px;"
      "text-shadow:0 0 12px #3aff5e44;white-space:pre}"
      ".hex{margin-top:28px;font:26px monospace;color:#8ab}"
      ".hex b{color:#fff}"
      "table{margin-top:20px;border-collapse:collapse;font:24px monospace}"
      "td{padding:4px 20px;color:#7a8;border-bottom:1px solid #222;text-align:left}"
      "td.bit{color:#9bd}"
      "tr.on td{color:#3aff5e}"
      "td.unk{color:#e55}"
      // ---- button bank: three columns, width fit-content (a fixed 40ch overflowed rightward) ----
      ".bank{display:flex;justify-content:space-between;"
      "margin-top:28px;gap:12px}"
      ".bcol{display:flex;flex-direction:column;align-items:center;gap:10px}"
      ".btn{display:flex;align-items:center;justify-content:center;width:92px;"
      "height:56px;font:bold 24px monospace;color:#dfdfdf;background:#2a2a2a;"
      "border:3px solid #444;border-radius:12px;text-decoration:none;"
      "user-select:none;box-shadow:0 3px 0 #111}"
      ".btn:active{background:#3a3a3a;transform:translateY(2px);box-shadow:0 1px 0 #111}"
      ".btn.idle{color:#666;border-style:dashed;background:#202020;box-shadow:none}"
      ".knobwrap{display:flex;align-items:center;gap:8px}"
      ".knobbtn{width:52px;height:52px;font-size:26px;border-radius:50%}"
      ".knob{width:84px;height:84px;border-radius:50%;background:radial-gradient("
      "circle at 35% 35%,#3c3c3c,#181818);border:4px solid #555;position:relative;"
      "box-shadow:0 0 0 4px #101010,0 4px 10px #000}"
      ".knob .marker{position:absolute;left:50%;top:6px;width:6px;height:24px;"
      "background:#9fdca6;border-radius:3px;transform-origin:3px 36px}"
      ".banknote{margin-top:14px;font:16px sans-serif;color:#667}"
      "</style></head><body><div class=lcd>"

      // ---- symbol strip: annotation line above each symbol ----
      "<div class=syms>"
      // Kompressorsymbol A/B
      "<div class=col><div class=ann><span id=cA class=";
  page += comp_a ? "'lit'>A</span>" : "'dim'>A</span>";
  page += " <span id=cB class=";
  page += comp_b ? "'lit'>B</span>" : "'dim'>B</span>";
  page += "</div><div data-b='0.7' class=";
  page += comp_on ? "'on'>&#127744;</div></div>" : "'off'>&#127744;</div></div>";

  // Tillsatssymbol I/II/III (bits unmapped: annotations stay dim)
  page += "<div class=col><div class=ann><span class='dim'>I II III</span>"
          "</div><div class='off'>&#9889;</div></div>";

  // Varmvattensymbol A/B
  page += "<div class=col><div class=ann><span id=hA class=";
  page += hw_a ? "'lit'>A</span>" : "'dim'>A</span>";
  page += " <span id=hB class=";
  page += hw_b ? "'lit'>B</span>" : "'dim'>B</span>";
  page += "</div><div data-b='1.7' class=";
  page += hw_on ? "'on'>&#128703;</div></div>" : "'off'>&#128703;</div></div>";

  // Cirkulationspumpsymbol
  page += "<div class=col><div class=ann>&nbsp;</div><div data-b='2.7' class=";
  page += pump_on ? "'on'>&#128260;</div></div>" : "'off'>&#128260;</div></div>";

  // Värmesystemsymbol
  page += "<div class=col><div class=ann>&nbsp;</div><div data-b='2.3' class=";
  page += heat_on ? "'on'>&#127968;</div></div>" : "'off'>&#127968;</div></div>";

  // Avfrostningssymbol (bits unmapped: shown dim)
  page += "<div class=col><div class=ann>&nbsp;</div><div class='off'>&#10052;</div></div>";
  page += "</div>";
  // row 2: value — centered, larger
  page += "<div class=row1 id=rv>";
  web_append_escaped(page, row2.c_str());
  // row 3: label — left
  page += "</div><div class=row2 id=rl>";
  web_append_escaped(page, row3.c_str());
  // row 4: menu left, clock right
  page += "</div><div class=row3><span id=rm>";
  web_append_escaped(page, r4l.c_str());
  page += "</span><span id=rc>";
  web_append_escaped(page, r4r.c_str());
  page += "</span></div></div>";

  // ---- button bank: col 1 placeholders, col 2 knob, col 3 plus/minus/enter (fetch() actions) ----
  // knob dial: vertical at 127; one detent (12 placements) = 9 degrees — 10 detents = +/-90 degrees
  char rot[48];
  int knob_deg = (int) this->knob_pos_ - 127;
  knob_deg = knob_deg * 3 / 4;
  snprintf(rot, sizeof(rot), "\"transform:rotate(%ddeg)\"", knob_deg);
  page += "<div class=bank>"
          "<div class=bcol>"
          "<div class=\"btn idle\" onclick=\"k('b1')\">&mdash;</div>"
          "<div class=\"btn idle\" onclick=\"k('b2')\">&mdash;</div>"
          "<div class=\"btn idle\" onclick=\"k('b3')\">&mdash;</div>"
          "</div>"
          "<div class=bcol><div class=knobwrap>"
          "<div class=\"btn knobbtn\" onclick=\"k('kl')\">&#10226;</div>"
          "<div class=knob><div class=marker style=";
  page += rot;
  page += "></div></div>"
          "<div class=\"btn knobbtn\" onclick=\"k('kr')\">&#10227;</div>"
          "</div></div>"
          "<div class=bcol>"
          "<div class=btn onclick=\"k('plus')\">&plus;</div>"
          "<div class=btn onclick=\"k('minus')\">&minus;</div>"
          "<div class=btn onclick=\"k('enter')\">OK</div>"
          "</div>"
          "</div>"
          "<div class=banknote>keys deliver on the next bus poll; the display "
          "updates as content changes</div>";
  page += "<div class=hex>symbol strip raw: <b id=hx>";

  // raw bytes
  if (this->sym_raw_len_ == 0) {
    page += "-- no display data yet --";
  } else {
    char hex[8];
    for (uint8_t i = 0; i < this->sym_raw_len_; i++) {
      snprintf(hex, sizeof(hex), "%s%02X", i ? " " : "", (unsigned) this->sym_raw_[i]);
      page += hex;
    }
  }
  page += "</b></div>";

  // bit table for matching yet-unknown symbols
  page += "<table>";
  char buf[64];
  for (const auto &b : WEB_BITS) {
    bool on = this->sym_raw_len_ > b.byte && ((this->sym_raw_[b.byte] >> b.bit) & 1);
    bool confirmed_unmapped = !b.confirmed && b.label[0] == '?';
    snprintf(buf, sizeof(buf), "<tr class=%s data-b='%u.%u'><td class=bit>b%u.%u</td><td", on ? "on" : "off",
             (unsigned) b.byte, (unsigned) b.bit, (unsigned) b.byte, (unsigned) b.bit);
    page += buf;
    if (confirmed_unmapped)
      page += " class=unk";
    page += ">";
    web_append_escaped(page, b.label);
    page += on ? " &lt;ON&gt;</td></tr>" : "</td></tr>";
  }
  page += "</table>";
  page += "<div class=hex>&#9889; I/II/III and &#10052; are shown dim until their bits are mapped — "
          "watch the red ? bits and report which panel symbol lit up</div>";

  // ---- live update: /j poll every 1000 ms; any press switches to 2 s of 120 ms fast polling ----
  page +=
      "<script>"
      "var busy=false,fastUntil=0;"
      "function k(a){fetch('/?k='+a);fastUntil=Date.now()+2000;tick();}"
      "function s(i,v){var e=document.getElementById(i);if(e&&e.textContent!=v)e.textContent=v;}"
      "function loop(){"
      "if(busy)return;"
      "var wait=(Date.now()<fastUntil)?120:1000;"
      "setTimeout(tick,wait);"
      "}"
      "function tick(){"
      "if(busy){loop();return;}"
      "busy=true;"
      "fetch('/j').then(function(r){return r.json();}).then(function(d){"
      "s('rv',d.v);s('rl',d.l);s('rm',d.m);s('rc',d.c);s('hx',d.hx);"
      "var st=document.querySelector('.marker');"
      "if(st)st.style.transform='rotate('+((d.k-127)*3/4)+'deg)';"
      "var b=d.hx.split(' ');var by=[];"
      "for(var i=0;i<b.length;i++)by[i]=parseInt(b[i],16)||0;"
      "function bit(o,m){return by.length>o&&(by[o]&m)!=0;}"
      "function cls(i,on){var e=document.getElementById(i);if(e)e.className=on?'lit':'dim';}"
      "cls('cA',bit(0,0x40));cls('cB',bit(0,0x20));cls('hA',bit(1,0x40));cls('hB',bit(1,0x20));"
      "var nodes=document.querySelectorAll('[data-b]');"
      "for(var i=0;i<nodes.length;i++){"
      "var p=nodes[i].getAttribute('data-b').split('.');"
      "var o=parseInt(p[0],10),t=parseInt(p[1],10);"
      "nodes[i].className=(by.length>o&&((by[o]>>t)&1)==1)?'on':'off';"
      "}"
      "busy=false;loop();"
      "}).catch(function(){busy=false;loop();});"
      "}"
      "loop();"
      "</script>";
  page += "</body></html>";
  return page;
}

void NibeBus::web_handle_action_(const std::string &action) {
  if (action == "plus") {
    ESP_LOGI(TAG, "display-web: plus");
    this->queue_key(WEB_KEY_PLUS);
  } else if (action == "minus") {
    ESP_LOGI(TAG, "display-web: minus");
    this->queue_key(WEB_KEY_MINUS);
  } else if (action == "enter") {
    ESP_LOGI(TAG, "display-web: enter");
    this->queue_key(WEB_KEY_ENTER);
  } else if (action == "kl") {  // knob one detent counter-clockwise
    uint8_t pos = this->knob_pos_ > WEB_KNOB_STEP ? this->knob_pos_ - WEB_KNOB_STEP : 0;
    ESP_LOGI(TAG, "display-web: knob left -> %u", (unsigned) pos);
    this->set_knob_placement(pos);
  } else if (action == "kr") {  // knob one detent clockwise
    uint8_t pos = this->knob_pos_ < 255 - WEB_KNOB_STEP ? this->knob_pos_ + WEB_KNOB_STEP : 255;
    ESP_LOGI(TAG, "display-web: knob right -> %u", (unsigned) pos);
    this->set_knob_placement(pos);
  } else if (action == "b1" || action == "b2" || action == "b3") {
    // reserved buttons: no function yet (slots for future page actions)
    ESP_LOGI(TAG, "display-web: %s (reserved, no action)", action.c_str());
  }
}

void NibeBus::serve_display_web() {
  if (this->web_port_ == 0)
    return;

  // ---- keep a client moving ----
  if (this->web_client_ != nullptr) {
    uint32_t now = millis();
    if (now - this->web_client_ms_ > 8000) {  // stuck connection: drop it
      this->web_client_.reset();
      this->web_req_complete_ = false;
      this->web_sent_ = 0;
      return;
    }
    if (!this->web_req_complete_) {
      // drain the request until end-of-headers (a GET has no body)
      while (this->web_request_.size() < 1024) {
        uint8_t buf[128];
        ssize_t n = this->web_client_->read(buf, sizeof(buf));
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
          return;  // more may come; keep draining next loop
        if (n <= 0)
          break;  // EOF/error: respond anyway or drop; respond below
        this->web_client_ms_ = millis();  // active connection: not "stuck"
        this->web_request_.append((const char *) buf, n);
        if (this->web_request_.find("\r\n\r\n") != std::string::npos)
          break;
      }
      if (this->web_request_.find("\r\n\r\n") == std::string::npos && this->web_request_.size() < 1024 &&
          millis() - this->web_client_ms_ < 2000)
        return;  // keep waiting for the request
      // routing: GET /j = JSON snapshot; GET /?k=.. = page action (204 via fetch(), no navigation); GET / = the page.
      size_t q = this->web_request_.find("?k=");
      if (q != std::string::npos) {
        std::string act = this->web_request_.substr(q + 3, 8);
        size_t end = act.find_first_of(" &");
        if (end != std::string::npos)
          act.resize(end);
        this->web_handle_action_(act);
        this->web_response_ =
            "HTTP/1.1 204 No Content\r\nCache-Control: no-store\r\n"
            "Connection: close\r\n\r\n";
        this->web_sent_ = 0;
        this->web_req_complete_ = true;
        return;  // stream it on the next loop pass
      }
      std::string body;
      const char *ctype = "text/html; charset=utf-8";
      if (this->web_request_.compare(0, 6, "GET /j") == 0) {
        body = this->build_display_json_();
        ctype = "application/json";
      } else {
        body = this->build_display_page_();
      }
      this->web_response_ = "HTTP/1.1 200 OK\r\nContent-Type: ";
      this->web_response_ += ctype;
      this->web_response_ += "\r\n";
      char len[32];
      snprintf(len, sizeof(len), "Content-Length: %u\r\n", (unsigned) body.size());
      this->web_response_ += len;
      this->web_response_ += "Connection: close\r\nCache-Control: no-store\r\n\r\n";
      this->web_response_ += body;
      this->web_sent_ = 0;
      this->web_req_complete_ = true;
    }
    // stream the response out
    while (this->web_sent_ < this->web_response_.size()) {
      ssize_t n =
          this->web_client_->write((const uint8_t *) this->web_response_.data() + this->web_sent_,
                                   this->web_response_.size() - this->web_sent_);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return;  // socket full: continue next loop
      if (n <= 0) {
        this->web_client_.reset();  // send error: drop
        this->web_req_complete_ = false;
        this->web_sent_ = 0;
        return;
      }
      this->web_client_ms_ = millis();  // active connection: not "stuck"
      this->web_sent_ += n;
    }
    // done: close connection cleanly (request fully drained beforehand)
    this->web_client_.reset();
    this->web_req_complete_ = false;
    this->web_request_.clear();
    this->web_response_.clear();
    this->web_sent_ = 0;
    return;
  }
  this->web_request_.clear();

  // ---- listener ----
  if (this->web_listen_ == nullptr) {
    if (millis() - this->web_listen_try_ms_ < 5000)
      return;
    this->web_listen_try_ms_ = millis();
    auto s = socket::socket_ip_loop_monitored(SOCK_STREAM, IPPROTO_TCP);
    if (s == nullptr)
      return;
    int yes = 1;
    s->setsockopt(SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_storage local;
    socklen_t local_len =
        socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&local), sizeof(local), this->web_port_);
    if (s->setblocking(false) != 0 || local_len == 0 ||
        s->bind(reinterpret_cast<struct sockaddr *>(&local), local_len) != 0 || s->listen(1) != 0) {
      int err = errno;
      ESP_LOGW(TAG, "display-web: listen on %u failed: %d", (unsigned) this->web_port_, err);
      return;
    }
    this->web_listen_ = std::move(s);
    ESP_LOGI(TAG, "display-web: virtual display on port %u", (unsigned) this->web_port_);
    return;
  }

  // ---- accept a new client ----
  if (!this->web_listen_->ready())
    return;
  struct sockaddr_storage peer;
  socklen_t peer_len = sizeof(peer);
  auto client = this->web_listen_->accept_loop_monitored(reinterpret_cast<struct sockaddr *>(&peer), &peer_len);
  if (client == nullptr)
    return;
  client->setblocking(false);
  this->web_client_ = std::move(client);
  this->web_client_ms_ = millis();
  this->web_req_complete_ = false;
  this->web_request_.clear();
}

}  // namespace nibe
}  // namespace esphome

#endif  // USE_NIBE_DISPLAY_WEB
