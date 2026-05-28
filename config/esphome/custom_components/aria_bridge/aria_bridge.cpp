#include "aria_bridge.h"
#include "esphome/core/log.h"

#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <cerrno>

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
  }
}
void ARIABridge::fire_thinking_() {
  if (this->led_phase_.exchange(LedPhase::THINKING) != LedPhase::THINKING) {
    ESP_LOGD(TAG, "led: -> THINKING");
    this->on_thinking_start_cb_.call();
  }
}
void ARIABridge::fire_responding_() {
  if (this->led_phase_.exchange(LedPhase::RESPONDING) != LedPhase::RESPONDING) {
    ESP_LOGD(TAG, "led: -> RESPONDING");
    this->on_responding_start_cb_.call();
  }
}
void ARIABridge::fire_error_() {
  if (this->led_phase_.exchange(LedPhase::ERROR_PHASE) != LedPhase::ERROR_PHASE) {
    ESP_LOGD(TAG, "led: -> ERROR");
    this->on_error_cb_.call();
  }
}
void ARIABridge::fire_idle_() {
  if (this->led_phase_.exchange(LedPhase::IDLE) != LedPhase::IDLE) {
    ESP_LOGD(TAG, "led: -> IDLE");
    this->on_idle_cb_.call();
  }
}

void ARIABridge::setup() {
  ESP_LOGI(TAG, "ARIA Bridge initialized — URL: %s", this->bridge_url_.c_str());

  this->mic_->add_data_callback([this](const std::vector<uint8_t> &data) {
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

  char request[512];
  snprintf(request, sizeof(request),
    "GET %s HTTP/1.1\r\n"
    "Host: %s:%d\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n",
    path.c_str(), host.c_str(), port, ws_key);

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
  this->state_ = BridgeState::IDLE;
  this->fire_idle_();                   // v21: turn LED off at the end of every session
}

}  // namespace aria_bridge
}  // namespace esphome
