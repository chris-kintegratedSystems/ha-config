#include "aria_bridge.h"
#include "esphome/core/log.h"

#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <sys/time.h>     // v23: gettimeofday for ts_unix_ms
#include <esp_system.h>   // v23: esp_reset_reason
#include <esp_heap_caps.h>

namespace esphome {
namespace aria_bridge {

static const char *const TAG = "aria_bridge";

// v21: fire callback ONLY when the phase actually changes. atomic<>::exchange returns the old
// value, so the callback is dispatched at most once per LISTENING→THINKING→RESPONDING transition
// (multiple binary audio frames after first won't re-fire on_responding_start, etc).
void ARIABridge::fire_listening_() {
  if (this->led_phase_.exchange(LedPhase::LISTENING) != LedPhase::LISTENING) {
    ESP_LOGD(TAG, "led: -> LISTENING");
    this->on_listening_start_cb_.call();
    this->emit_event("led_phase_change", "{\"to\":\"listening\"}");
  }
}
void ARIABridge::fire_thinking_() {
  if (this->led_phase_.exchange(LedPhase::THINKING) != LedPhase::THINKING) {
    ESP_LOGD(TAG, "led: -> THINKING");
    this->on_thinking_start_cb_.call();
    this->emit_event("led_phase_change", "{\"to\":\"thinking\"}");
  }
}
void ARIABridge::fire_responding_() {
  if (this->led_phase_.exchange(LedPhase::RESPONDING) != LedPhase::RESPONDING) {
    ESP_LOGD(TAG, "led: -> RESPONDING");
    this->on_responding_start_cb_.call();
    this->emit_event("led_phase_change", "{\"to\":\"responding\"}");
  }
}
void ARIABridge::fire_error_() {
  if (this->led_phase_.exchange(LedPhase::ERROR_PHASE) != LedPhase::ERROR_PHASE) {
    ESP_LOGD(TAG, "led: -> ERROR");
    this->on_error_cb_.call();
    this->emit_event("led_phase_change", "{\"to\":\"error\"}");
  }
}
void ARIABridge::fire_idle_() {
  if (this->led_phase_.exchange(LedPhase::IDLE) != LedPhase::IDLE) {
    ESP_LOGD(TAG, "led: -> IDLE");
    this->on_idle_cb_.call();
    this->emit_event("led_phase_change", "{\"to\":\"idle\"}");
  }
}

// v23: enqueue an outgoing event; the worker task drains and POSTs. Capped at 64 in-flight
// events to bound PSRAM use under bursty conditions.
void ARIABridge::emit_event(const std::string &event_type, const std::string &payload_json) {
  if (this->event_post_url_.empty()) return;
  OutgoingEvent e;
  e.event = event_type;
  e.payload_json = payload_json.empty() ? "{}" : payload_json;
  e.session_id = this->session_uuid_;
  {
    std::lock_guard<std::mutex> lock(this->event_queue_mutex_);
    if (this->event_queue_.size() >= 64) this->event_queue_.erase(this->event_queue_.begin());
    this->event_queue_.push_back(std::move(e));
  }
  TaskHandle_t h = this->event_post_task_handle_.load();
  if (h != nullptr) xTaskNotifyGive(h);
}

// v23: worker — drains the queue and POSTs each event. Wakes on notify or every 1s.
void ARIABridge::event_post_task_(void *param) {
  ARIABridge *self = static_cast<ARIABridge *>(param);
  ESP_LOGI(TAG, "event_post_task started -> %s", self->event_post_url_.c_str());
  while (self->event_post_task_running_.load()) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    while (true) {
      OutgoingEvent ev;
      {
        std::lock_guard<std::mutex> lock(self->event_queue_mutex_);
        if (self->event_queue_.empty()) break;
        ev = std::move(self->event_queue_.front());
        self->event_queue_.erase(self->event_queue_.begin());
      }
      // ts_unix_ms from gettimeofday (valid once SNTP has synced; pre-sync = millis-since-boot)
      struct timeval tv; gettimeofday(&tv, nullptr);
      uint64_t ts_ms = (uint64_t) tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
      char envelope[1024];
      const char *sid_open = ev.session_id.empty() ? "null" : "\"";
      const char *sid_close = ev.session_id.empty() ? "" : "\"";
      int n = snprintf(envelope, sizeof(envelope),
                       "{\"source\":\"satellite\",\"event\":\"%s\",\"session_id\":%s%s%s,"
                       "\"ts_unix_ms\":%llu,\"satellite_id\":\"%s\",\"schema\":1,\"payload\":%s}",
                       ev.event.c_str(),
                       sid_open, ev.session_id.c_str(), sid_close,
                       (unsigned long long) ts_ms,
                       self->satellite_id_.c_str(),
                       ev.payload_json.c_str());
      if (n > 0 && n < (int) sizeof(envelope)) {
        self->http_post_json_(self->event_post_url_, std::string(envelope));
      }
    }
  }
  self->event_post_task_handle_.store(nullptr);
  vTaskDelete(nullptr);
}

// v23: minimal HTTP/1.1 POST helper. Fire-and-forget — doesn't read response body, just sends
// the request and closes. Synchronous; ~10–50 ms on LAN. Used from the event_post_task_ and the
// prewake_upload_task_ — never from the audio/wake-detect critical path.
bool ARIABridge::http_post_json_(const std::string &url, const std::string &body) const {
  // url like "http://192.168.51.179:8766/satellite-event" — parse host/port/path inline.
  if (url.substr(0, 7) != "http://") return false;
  std::string rest = url.substr(7);
  size_t slash = rest.find('/');
  std::string host_port = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
  std::string path = (slash != std::string::npos) ? rest.substr(slash) : "/";
  uint16_t port = 80;
  size_t colon = host_port.find(':');
  std::string host = host_port;
  if (colon != std::string::npos) {
    host = host_port.substr(0, colon);
    port = static_cast<uint16_t>(std::stoi(host_port.substr(colon + 1)));
  }
  struct hostent *he = gethostbyname(host.c_str());
  if (!he) return false;
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return false;
  struct sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
  memcpy(&addr.sin_addr, he->h_addr, he->h_length);
  struct timeval to; to.tv_sec = 2; to.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
  if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) { lwip_close(fd); return false; }
  char header[256];
  int hn = snprintf(header, sizeof(header),
                    "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: application/json\r\n"
                    "Content-Length: %u\r\nConnection: close\r\n\r\n",
                    path.c_str(), host.c_str(), (unsigned) port, (unsigned) body.size());
  bool ok = (lwip_send(fd, header, hn, 0) == hn) &&
            (body.empty() || lwip_send(fd, body.data(), body.size(), 0) == (int) body.size());
  lwip_close(fd);
  return ok;
}

bool ARIABridge::http_post_binary_(const std::string &url, const uint8_t *body, size_t len) const {
  if (url.substr(0, 7) != "http://") return false;
  std::string rest = url.substr(7);
  size_t slash = rest.find('/');
  std::string host_port = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
  std::string path = (slash != std::string::npos) ? rest.substr(slash) : "/";
  uint16_t port = 80;
  size_t colon = host_port.find(':');
  std::string host = host_port;
  if (colon != std::string::npos) {
    host = host_port.substr(0, colon);
    port = static_cast<uint16_t>(std::stoi(host_port.substr(colon + 1)));
  }
  struct hostent *he = gethostbyname(host.c_str());
  if (!he) return false;
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return false;
  struct sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
  memcpy(&addr.sin_addr, he->h_addr, he->h_length);
  struct timeval to; to.tv_sec = 8; to.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
  if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) { lwip_close(fd); return false; }
  char header[256];
  int hn = snprintf(header, sizeof(header),
                    "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: application/octet-stream\r\n"
                    "Content-Length: %u\r\nConnection: close\r\n\r\n",
                    path.c_str(), host.c_str(), (unsigned) port, (unsigned) len);
  bool ok = (lwip_send(fd, header, hn, 0) == hn);
  // Chunked send in 4 kB chunks to avoid pressure on small SNDBUF.
  const size_t CHUNK = 4096;
  for (size_t off = 0; ok && off < len; off += CHUNK) {
    size_t n = std::min(CHUNK, len - off);
    if (lwip_send(fd, body + off, n, 0) != (int) n) { ok = false; break; }
  }
  lwip_close(fd);
  return ok;
}

// v23: pre-wake upload task — runs once per session. Owns its data buffer (passed via heap
// struct), POSTs it, frees, exits.
struct PrewakeUploadArg {
  ARIABridge *self;
  std::string url;                  // includes session_id + format query string
  std::vector<uint8_t> data;
};
void ARIABridge::prewake_upload_task_(void *param) {
  PrewakeUploadArg *arg = static_cast<PrewakeUploadArg *>(param);
  ESP_LOGI(TAG, "prewake upload: %u bytes -> %s", (unsigned) arg->data.size(), arg->url.c_str());
  bool ok = arg->self->http_post_binary_(arg->url, arg->data.data(), arg->data.size());
  ESP_LOGI(TAG, "prewake upload: %s", ok ? "OK" : "FAIL");
  delete arg;
  vTaskDelete(nullptr);
}

void ARIABridge::start_session_with_uuid(const std::string &uuid) {
  this->session_uuid_ = uuid;
  // Snapshot the pre-wake ring NOW (before the WS comes up and starts consuming new mic data).
  // Spawn a task to POST it so the wake-handler lambda doesn't block.
  std::vector<uint8_t> snap = this->snapshot_prewake();
  if (!this->prewake_post_url_.empty() && !snap.empty()) {
    char url_buf[256];
    snprintf(url_buf, sizeof(url_buf), "%s?session_id=%s&format=int32_stereo_16k",
             this->prewake_post_url_.c_str(), uuid.c_str());
    auto *arg = new PrewakeUploadArg();
    arg->self = this;
    arg->url = url_buf;
    arg->data = std::move(snap);
    xTaskCreatePinnedToCore(prewake_upload_task_, "aria_pwup", 4096, arg, 3, nullptr, 0);
  }
  this->emit_event("aria_session_started", "{}");
  this->start_session();
}

void ARIABridge::setup() {
  ESP_LOGI(TAG, "ARIA Bridge initialized — URL: %s", this->bridge_url_.c_str());

  // v23: pre-wake ring — sized for ~prewake_seconds_ of 16 kHz stereo int32 (the format the
  // FPH satellite1 mic platform feeds add_data_callback). 16000 × 8 × 2 ≈ 256 kB by default.
  // Sized generously so larger raw formats still fit.
  size_t ring_bytes = static_cast<size_t>(this->prewake_seconds_) * 16000 * 8;
  if (ring_bytes < 64 * 1024) ring_bytes = 64 * 1024;
  this->prewake_ring_.assign(ring_bytes, 0);
  ESP_LOGI(TAG, "v23 prewake ring: %u bytes in PSRAM (~%ds @ 16kHz stereo int32)",
           (unsigned) ring_bytes, this->prewake_seconds_);

  this->mic_->add_data_callback([this](const std::vector<uint8_t> &data) {
    // v23: ALWAYS write to the prewake ring (independent of session state) — that's the whole
    // point of pre-wake: capture what was in the air BEFORE the wake word fired.
    this->prewake_ring_write_(data.data(), data.size());

    if (this->state_ != BridgeState::STREAMING || this->sock_fd_ < 0)
      return;
    std::lock_guard<std::mutex> lock(this->mic_mutex_);
    if (this->mic_buffer_.size() + data.size() > MAX_MIC_BUFFER_BYTES) {
      this->mic_dropped_bytes_ += data.size();  // v11: count, don't spam-log per drop
      return;
    }
    this->mic_buffer_.insert(this->mic_buffer_.end(), data.begin(), data.end());
    TaskHandle_t h = this->send_task_handle_.load();
    if (h != nullptr)
      xTaskNotifyGive(h);
  });

  this->mic_buffer_.reserve(MAX_MIC_BUFFER_BYTES);
  ESP_LOGI(TAG, "Mic buffer: reserved %u bytes in PSRAM", (unsigned) MAX_MIC_BUFFER_BYTES);

  // v23: start the event-post worker task. Drains the outgoing event queue → HTTP POST to bridge.
  this->event_post_task_running_ = true;
  TaskHandle_t evt_handle = nullptr;
  xTaskCreatePinnedToCore(event_post_task_, "aria_evtpost", 4096, this, 4, &evt_handle, 0);
  this->event_post_task_handle_.store(evt_handle);

  // v23: emit boot event so the timeline starts before any session
  char body[160];
  snprintf(body, sizeof(body),
           "{\"reset_reason\":%d,\"free_psram_kb\":%u,\"prewake_seconds\":%d}",
           (int) esp_reset_reason(), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           this->prewake_seconds_);
  this->emit_event("boot", body);
}

// v23: ring write — overwrites oldest bytes when full. Cheap; called from the audio callback.
void ARIABridge::prewake_ring_write_(const uint8_t *data, size_t len) {
  if (this->prewake_ring_.empty()) return;
  std::lock_guard<std::mutex> lock(this->prewake_mutex_);
  const size_t cap = this->prewake_ring_.size();
  if (len >= cap) {
    std::memcpy(this->prewake_ring_.data(), data + (len - cap), cap);
    this->prewake_head_ = 0;
    this->prewake_full_ = true;
    return;
  }
  size_t first = std::min(len, cap - this->prewake_head_);
  std::memcpy(this->prewake_ring_.data() + this->prewake_head_, data, first);
  if (len > first) {
    std::memcpy(this->prewake_ring_.data(), data + first, len - first);
    this->prewake_full_ = true;
  }
  this->prewake_head_ += len;
  if (this->prewake_head_ >= cap) {
    this->prewake_head_ -= cap;
    this->prewake_full_ = true;
  }
}

std::vector<uint8_t> ARIABridge::snapshot_prewake() {
  std::lock_guard<std::mutex> lock(this->prewake_mutex_);
  std::vector<uint8_t> out;
  if (this->prewake_ring_.empty()) return out;
  if (!this->prewake_full_) {
    out.assign(this->prewake_ring_.begin(), this->prewake_ring_.begin() + this->prewake_head_);
  } else {
    out.reserve(this->prewake_ring_.size());
    out.insert(out.end(), this->prewake_ring_.begin() + this->prewake_head_, this->prewake_ring_.end());
    out.insert(out.end(), this->prewake_ring_.begin(), this->prewake_ring_.begin() + this->prewake_head_);
  }
  return out;
}

void ARIABridge::loop() {
  if (this->state_ == BridgeState::IDLE)
    return;

  if (this->state_ == BridgeState::CONNECTING)
    return;

  if (this->state_ == BridgeState::ERROR) {
    ESP_LOGW(TAG, "Connection error — stopping session");
    this->fire_error_();     // v21: flash red briefly before stop_session drives back to IDLE
    this->stop_session();
    return;
  }

  uint32_t now = millis();

  if (now - this->last_activity_ms_.load() > SESSION_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Session timeout — stopping");
    this->stop_session();
    return;
  }

  // Receive bridge audio (send moved to send_task_).
  // v18 drain fix: feed the speaker with as many WS frames as are available per loop() iteration.
  // Previously one ~5ms frame/loop fed ~3x slower than real-time -> the 500ms speaker buffer
  // underran -> stutter. spk_pending_ holds any bytes the buffer couldn't accept (backpressure;
  // no drop, no blocking — we just stop reading until the buffer drains).
  if (this->sock_fd_ >= 0) {
#ifdef USE_SPEAKER
    if (this->spk_ != nullptr && !this->spk_pending_.empty()) {
      this->spk_->start();
      size_t w = this->spk_->play(this->spk_pending_.data(), this->spk_pending_.size());
      if (w > 0) {
        this->spk_pending_.erase(this->spk_pending_.begin(), this->spk_pending_.begin() + w);
        this->last_spk_play_ms_ = now;     // v20: i2s buffer received more bytes — reset drain clock
      }
    }
    bool can_read = (this->spk_ == nullptr) || this->spk_pending_.empty();
#else
    bool can_read = true;
#endif
    uint8_t buf[2048];
    for (int n = 0; n < 24 && can_read; n++) {
      int len = this->ws_recv_frame_(buf, sizeof(buf));
      if (len > 0) {
        this->last_activity_ms_ = now;
        this->state_ = BridgeState::RECEIVING;
        this->playback_complete_sent_ = false;  // v20: new audio arrived — invalidate any stale "drained" state
        this->fire_responding_();               // v21: TTS audio is arriving → RESPONDING (no-op after first frame)
#ifdef USE_SPEAKER
        if (this->spk_ != nullptr) {
          this->spk_->start();
          size_t w = this->spk_->play(buf, static_cast<size_t>(len));
          if (w > 0)
            this->last_spk_play_ms_ = now;  // v20: i2s buffer received bytes (even partial)
          if (w < static_cast<size_t>(len)) {
            this->spk_pending_.assign(buf + w, buf + len);  // hold remainder; stop reading (backpressure)
            break;
          }
        }
#endif
      } else if (len < 0) {
        ESP_LOGE(TAG, "WebSocket read error — stopping");
        this->stop_session();
        break;
      } else {
        break;  // no more WS frames available this iteration
      }
    }

#ifdef USE_SPEAKER
    // v20: emit playback_complete once when the i2s buffer has had time to drain
    // (PLAYBACK_IDLE_MS = i2s buffer + acoustic margin). The bridge keeps the half-duplex
    // gate closed until it sees this, so the satellite mic doesn't echo its own TTS into
    // server_vad and trigger a runaway response chain. .exchange(true) makes the send
    // exactly-once per playback session; reset on next spk_->play() or on stop_session.
    if (this->spk_ != nullptr) {
      uint32_t lp = this->last_spk_play_ms_.load();
      if (lp > 0 && this->spk_pending_.empty() &&
          (now - lp) >= PLAYBACK_IDLE_MS &&
          !this->playback_complete_sent_.exchange(true)) {
        this->send_playback_complete_();
      }
    }
#endif
  }
}

bool ARIABridge::parse_url_(std::string &host, uint16_t &port, std::string &path) {
  std::string url = this->bridge_url_;
  if (url.substr(0, 5) == "ws://") {
    url = url.substr(5);
    port = 80;
  } else if (url.substr(0, 6) == "wss://") {
    url = url.substr(6);
    port = 443;
  } else {
    return false;
  }

  size_t slash = url.find('/');
  std::string host_port = (slash != std::string::npos) ? url.substr(0, slash) : url;
  path = (slash != std::string::npos) ? url.substr(slash) : "/";

  size_t colon = host_port.find(':');
  if (colon != std::string::npos) {
    host = host_port.substr(0, colon);
    port = static_cast<uint16_t>(std::stoi(host_port.substr(colon + 1)));
  } else {
    host = host_port;
  }
  return true;
}

bool ARIABridge::tcp_connect_() {
  std::string host;
  uint16_t port;
  std::string path;
  if (!this->parse_url_(host, port, path)) {
    ESP_LOGE(TAG, "Invalid URL: %s", this->bridge_url_.c_str());
    return false;
  }

  struct hostent *he = gethostbyname(host.c_str());
  if (!he) {
    ESP_LOGE(TAG, "DNS failed for %s", host.c_str());
    return false;
  }

  this->sock_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (this->sock_fd_ < 0) {
    ESP_LOGE(TAG, "Socket create failed");
    return false;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  memcpy(&addr.sin_addr, he->h_addr, he->h_length);

  struct timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(this->sock_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(this->sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (connect(this->sock_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "TCP connect failed to %s:%d", host.c_str(), port);
    lwip_close(this->sock_fd_);
    this->sock_fd_ = -1;
    return false;
  }

  ESP_LOGI(TAG, "TCP connected to %s:%d", host.c_str(), port);
  return true;
}

bool ARIABridge::ws_handshake_() {
  std::string host;
  uint16_t port;
  std::string path;
  this->parse_url_(host, port, path);

  const char *ws_key = "dGhlIHNhbXBsZSBub25jZQ==";

  // v23: pass our session_uuid_ to the bridge so the satellite-side and bridge-side records
  // stitch on the SAME session_id (the prewake POST already wrote under this UUID).
  std::string path_with_qs = path;
  if (!this->session_uuid_.empty()) {
    path_with_qs += (path.find('?') == std::string::npos ? "?" : "&");
    path_with_qs += "session_id=";
    path_with_qs += this->session_uuid_;
  }
  char request[640];
  snprintf(request, sizeof(request),
    "GET %s HTTP/1.1\r\n"
    "Host: %s:%d\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n",
    path_with_qs.c_str(), host.c_str(), port, ws_key);

  if (lwip_send(this->sock_fd_, request, strlen(request), 0) < 0) {
    ESP_LOGE(TAG, "WS handshake send failed");
    return false;
  }

  char response[512];
  int received = 0;
  while (received < (int)sizeof(response) - 1) {
    int r = lwip_recv(this->sock_fd_, response + received, 1, 0);
    if (r <= 0) break;
    received += r;
    if (received >= 4 &&
        response[received-4] == '\r' && response[received-3] == '\n' &&
        response[received-2] == '\r' && response[received-1] == '\n') {
      break;
    }
  }
  response[received] = '\0';

  if (strstr(response, "101") == nullptr) {
    ESP_LOGE(TAG, "WS handshake rejected: %.100s", response);
    return false;
  }

  int flags = fcntl(this->sock_fd_, F_GETFL, 0);
  fcntl(this->sock_fd_, F_SETFL, flags | O_NONBLOCK);

  ESP_LOGI(TAG, "WebSocket handshake complete");
  return true;
}

void ARIABridge::ws_disconnect_() {
  if (this->sock_fd_ >= 0) {
    uint8_t close_frame[] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
    lwip_send(this->sock_fd_, close_frame, sizeof(close_frame), 0);
    lwip_close(this->sock_fd_);
    this->sock_fd_ = -1;
  }
}

bool ARIABridge::ws_send_binary_(const uint8_t *data, size_t len) {
  if (this->sock_fd_ < 0) return false;

  uint8_t header[4];
  size_t header_len;

  header[0] = 0x82;
  if (len < 126) {
    header[1] = static_cast<uint8_t>(len | 0x80);
    header_len = 2;
  } else {
    header[1] = 126 | 0x80;
    header[2] = static_cast<uint8_t>(len >> 8);
    header[3] = static_cast<uint8_t>(len & 0xFF);
    header_len = 4;
  }

  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};

  auto send_all = [this](const void *buf, size_t n) -> bool {
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    size_t remaining = n;
    while (remaining > 0) {
      int sent = lwip_send(this->sock_fd_, p, remaining, 0);
      if (sent > 0) {
        p += sent;
        remaining -= sent;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        vTaskDelay(pdMS_TO_TICKS(1));
      } else {
        return false;
      }
    }
    return true;
  };

  if (!send_all(header, header_len)) return false;
  if (!send_all(mask, 4)) return false;

  uint8_t *masked = static_cast<uint8_t *>(malloc(len));
  if (!masked) return false;
  for (size_t i = 0; i < len; i++)
    masked[i] = data[i] ^ mask[i % 4];
  bool ok = send_all(masked, len);
  free(masked);
  return ok;
}

// v20: send a WebSocket TEXT frame (opcode 0x1). Mirrors ws_send_binary_ framing/masking;
// kept as a separate method to avoid touching the proven binary-send path.
bool ARIABridge::ws_send_text_(const char *data, size_t len) {
  if (this->sock_fd_ < 0) return false;

  uint8_t header[4];
  size_t header_len;
  header[0] = 0x81;                                       // FIN + text opcode
  if (len < 126) {
    header[1] = static_cast<uint8_t>(len | 0x80);
    header_len = 2;
  } else {
    header[1] = 126 | 0x80;
    header[2] = static_cast<uint8_t>(len >> 8);
    header[3] = static_cast<uint8_t>(len & 0xFF);
    header_len = 4;
  }
  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};

  auto send_all = [this](const void *buf, size_t n) -> bool {
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    size_t remaining = n;
    while (remaining > 0) {
      int sent = lwip_send(this->sock_fd_, p, remaining, 0);
      if (sent > 0) {
        p += sent;
        remaining -= sent;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        vTaskDelay(pdMS_TO_TICKS(1));
      } else {
        return false;
      }
    }
    return true;
  };

  if (!send_all(header, header_len)) return false;
  if (!send_all(mask, 4)) return false;

  uint8_t *masked = static_cast<uint8_t *>(malloc(len));
  if (!masked) return false;
  for (size_t i = 0; i < len; i++)
    masked[i] = static_cast<uint8_t>(data[i]) ^ mask[i % 4];
  bool ok = send_all(masked, len);
  free(masked);
  return ok;
}

// v20: build + emit the playback_complete signal. Logged with the actual drain interval so
// post-mortem journal grep can verify it fired at the right moment.
void ARIABridge::send_playback_complete_() {
  char msg[80];
  uint32_t now = millis();
  int n = snprintf(msg, sizeof(msg), "{\"type\":\"playback_complete\",\"ts\":%lu}",
                   (unsigned long) now);
  if (n <= 0 || n >= (int) sizeof(msg)) return;
  if (this->ws_send_text_(msg, static_cast<size_t>(n))) {
    ESP_LOGI(TAG, "playback_complete sent (idle %lums)",
             (unsigned long) (now - this->last_spk_play_ms_.load()));
    // v23: also emit it as a structured event so the unified log carries the satellite-ts (the
    // WS msg goes to the bridge gate, this goes to the event log) for correlation across the
    // two channels.
    char p[64];
    snprintf(p, sizeof(p), "{\"idle_ms\":%lu}", (unsigned long)(now - this->last_spk_play_ms_.load()));
    this->emit_event("playback_complete", p);
  } else {
    ESP_LOGW(TAG, "playback_complete send failed");
    this->playback_complete_sent_ = false;     // allow retry on next loop iteration
  }
}

// Read exactly n bytes (handles partial TCP reads + EAGAIN with a bounded yield).
// Returns bytes read (== n on success). Guarantees frame fields are fully consumed
// so a partial read can never desync the WS frame stream (v16: fixes TTS clipping).
int ARIABridge::recv_exact_(uint8_t *buf, size_t n, uint32_t timeout_ms) {
  size_t total = 0;
  uint32_t start = millis();
  while (total < n) {
    int r = lwip_recv(this->sock_fd_, buf + total, n - total, MSG_DONTWAIT);
    if (r > 0) {
      total += r;
    } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (millis() - start > timeout_ms)
        break;
      vTaskDelay(pdMS_TO_TICKS(1));  // yield (don't busy-spin); rest of frame is in-flight
    } else {
      break;  // r == 0 (peer closed) or hard error
    }
  }
  return static_cast<int>(total);
}

int ARIABridge::ws_recv_frame_(uint8_t *buf, size_t max_len) {
  if (this->sock_fd_ < 0) return -1;

  // Non-blocking peek: only enter the (briefly blocking) atomic reads if data is present.
  uint8_t peek;
  int pr = lwip_recv(this->sock_fd_, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
  if (pr <= 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
  }

  // Read each field atomically — a short read here previously desynced the stream
  // (binary audio got parsed as text frames -> read error -> TTS cut off).
  uint8_t header[2];
  if (this->recv_exact_(header, 2, 100) != 2) return -1;

  uint8_t opcode = header[0] & 0x0F;
  bool masked = (header[1] & 0x80) != 0;
  size_t payload_len = header[1] & 0x7F;

  if (payload_len == 126) {
    uint8_t ext[2];
    if (this->recv_exact_(ext, 2, 100) != 2) return -1;
    payload_len = (static_cast<size_t>(ext[0]) << 8) | ext[1];
  } else if (payload_len == 127) {
    return -1;  // 64-bit lengths are never sent here; treat as desync
  }

  uint8_t mask_key[4] = {0};
  if (masked) {
    if (this->recv_exact_(mask_key, 4, 100) != 4) return -1;
  }

  if (payload_len == 0)
    return 0;
  if (payload_len > max_len)
    return -1;  // our frames are <=1024 B; larger => desync, fail cleanly

  if (this->recv_exact_(buf, payload_len, 300) != static_cast<int>(payload_len))
    return -1;
  size_t total = payload_len;

  if (masked) {
    for (size_t i = 0; i < total; i++)
      buf[i] ^= mask_key[i % 4];
  }

  switch (opcode) {
    case 0x08:  // close
      ESP_LOGI(TAG, "Received WS close frame");
      return -1;
    case 0x09: {  // ping -> pong
      uint8_t pong[6] = {0x8A, 0x80, 0x00, 0x00, 0x00, 0x00};
      lwip_send(this->sock_fd_, pong, sizeof(pong), 0);
      return 0;
    }
    case 0x0A:  // pong
      return 0;
    case 0x01: {  // text status from bridge
      std::string msg(reinterpret_cast<char *>(buf), total);
      ESP_LOGI(TAG, "Bridge status: %s", msg.c_str());
      if (msg.find("\"done\"") != std::string::npos) {
        this->state_ = BridgeState::STREAMING;  // ARIA finished — resume mic
        this->fire_listening_();                 // v21: back to listening for follow-up
      } else if (msg.find("\"processing\"") != std::string::npos) {
        this->state_ = BridgeState::RECEIVING;  // pause mic, listen for TTS (half-duplex)
        this->fire_thinking_();                  // v21: Grok is generating a response
      } else if (msg.find("\"timeout\"") != std::string::npos) {
        return -1;
      }
      return 0;
    }
    case 0x02:  // binary audio
      return static_cast<int>(total);
    default:
      return 0;  // continuation / unknown — ignore (don't desync)
  }
}

void ARIABridge::connect_task_(void *param) {
  ARIABridge *self = static_cast<ARIABridge *>(param);

  ESP_LOGI(TAG, "Connect task: TCP connecting...");
  if (!self->tcp_connect_()) {
    ESP_LOGE(TAG, "Connect task: TCP failed");
    self->state_ = BridgeState::ERROR;
    vTaskDelete(nullptr);
    return;
  }

  ESP_LOGI(TAG, "Connect task: WebSocket handshake...");
  if (!self->ws_handshake_()) {
    ESP_LOGE(TAG, "Connect task: WS handshake failed");
    self->ws_disconnect_();
    self->state_ = BridgeState::ERROR;
    vTaskDelete(nullptr);
    return;
  }

  self->state_ = BridgeState::STREAMING;
  ESP_LOGI(TAG, "Connect task: ARIA session active — streaming audio");
  vTaskDelete(nullptr);
}

void ARIABridge::send_task_(void *param) {
  ARIABridge *self = static_cast<ARIABridge *>(param);
  ESP_LOGI(TAG, "Send task started");

  while (self->send_task_running_.load()) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    if (!self->send_task_running_.load())
      break;

    if (self->state_ != BridgeState::STREAMING || self->sock_fd_ < 0)
      continue;

    std::vector<uint8_t> to_send;
    {
      std::lock_guard<std::mutex> lock(self->mic_mutex_);
      if (!self->mic_buffer_.empty()) {
        to_send.assign(self->mic_buffer_.begin(), self->mic_buffer_.end());
        self->mic_buffer_.clear();
      }
    }

    if (!to_send.empty()) {
      // v13: send in <=2048 B frames so each ws_send stays fast (criterion 4) and
      // the bridge reads smaller, more frequent frames (smoother, less backpressure).
      const size_t MAX_SEND = 2048;
      bool ok = true;
      for (size_t off = 0; off < to_send.size(); off += MAX_SEND) {
        size_t n = to_send.size() - off;
        if (n > MAX_SEND) n = MAX_SEND;
        uint32_t t0 = millis();
        ok = self->ws_send_binary_(to_send.data() + off, n);
        uint32_t dt = millis() - t0;  // flag slow sends (criterion 4)
        if (dt > 50)
          ESP_LOGW(TAG, "ws_send slow: %ums for %u bytes", (unsigned) dt, (unsigned) n);
        if (!ok)
          break;
        self->bytes_sent_ += n;
      }
      if (!ok) {
        if (self->send_task_running_.load()) {
          ESP_LOGE(TAG, "Send task: WebSocket send failed");
          self->state_ = BridgeState::ERROR;
        }
        break;
      }
      // v18 #3 fix: do NOT refresh last_activity_ms_ on mic-SEND. Continuous mic streaming
      // (incl. feedback) kept sessions alive forever (timeout never fired). last_activity_ms_
      // is now refreshed only on TTS-RECV (loop()), so a session ends ~SESSION_TIMEOUT_MS after
      // the last response — letting it end naturally instead of sticking open.
    }
  }

  ESP_LOGI(TAG, "Send task exiting");
  self->send_task_handle_.store(nullptr);
  vTaskDelete(nullptr);
}

void ARIABridge::start_session() {
  if (this->state_ != BridgeState::IDLE) {
    ESP_LOGW(TAG, "Session already active — ignoring wake word");
    return;
  }
  ESP_LOGI(TAG, "Wake word detected — starting ARIA session");
  this->fire_listening_();        // v21: light up immediately so the user sees feedback at wake (not after the WS handshake)
  this->state_ = BridgeState::CONNECTING;
  this->session_start_ms_ = millis();
  this->last_activity_ms_ = millis();
  this->mic_dropped_bytes_ = 0;  // v11: reset per-session stats
  this->bytes_sent_ = 0;
  this->last_spk_play_ms_ = 0;          // v20: no playback yet — guard send_playback_complete_
  this->playback_complete_sent_ = false;

  {
    std::lock_guard<std::mutex> lock(this->mic_mutex_);
    this->mic_buffer_.clear();
  }

#ifdef USE_SPEAKER
  if (this->spk_ != nullptr) {
    this->spk_->set_audio_stream_info(audio::AudioStreamInfo(16, 2, 48000));  // bridge sends 48kHz STEREO (I2S bus is stereo)
  }
#endif

  this->send_task_running_ = true;
  xTaskCreatePinnedToCore(
    connect_task_, "aria_conn", 8192, this, 5, nullptr, 0);
  TaskHandle_t handle = nullptr;
  xTaskCreatePinnedToCore(
    send_task_, "aria_send", 4096, this, 5, &handle, 0);
  this->send_task_handle_.store(handle);
}

void ARIABridge::stop_session() {
  ESP_LOGI(TAG, "Stopping ARIA session (duration: %lums)",
           (unsigned long)(millis() - this->session_start_ms_));
  ESP_LOGI(TAG, "Session stats: sent=%u bytes, dropped=%u bytes",
           (unsigned) this->bytes_sent_.load(), (unsigned) this->mic_dropped_bytes_.load());
  this->send_task_running_ = false;
  TaskHandle_t h = this->send_task_handle_.load();
  if (h != nullptr)
    xTaskNotifyGive(h);
  this->ws_disconnect_();
#ifdef USE_SPEAKER
  if (this->spk_ != nullptr) {
    this->spk_->finish();
  }
#endif
  this->spk_pending_.clear();  // v18: drop any held speaker remainder so it can't leak to the next session
  this->last_spk_play_ms_ = 0;          // v20: clean slate for the next session
  this->playback_complete_sent_ = false;
  // v23: emit aria_session_ended BEFORE clearing the session uuid (so the event carries it)
  {
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"duration_ms\":%lu,\"bytes_sent\":%u,\"bytes_dropped\":%u}",
             (unsigned long)(millis() - this->session_start_ms_),
             (unsigned) this->bytes_sent_.load(), (unsigned) this->mic_dropped_bytes_.load());
    this->emit_event("aria_session_ended", payload);
  }
  this->state_ = BridgeState::IDLE;
  this->fire_idle_();                   // v21: turn LED off at the end of every session
  this->session_uuid_.clear();          // v23: clear after final emit so next session starts fresh
}

}  // namespace aria_bridge
}  // namespace esphome
