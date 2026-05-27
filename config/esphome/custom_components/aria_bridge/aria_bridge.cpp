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
    this->stop_session();
    return;
  }

  uint32_t now = millis();

  if (now - this->last_activity_ms_.load() > SESSION_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Session timeout — stopping");
    this->stop_session();
    return;
  }

  // Receive bridge audio (send moved to send_task_)
  if (this->sock_fd_ >= 0) {
    uint8_t buf[2048];
    int len = this->ws_recv_frame_(buf, sizeof(buf));
    if (len > 0) {
#ifdef USE_SPEAKER
      if (this->spk_ != nullptr) {
        this->spk_->start();
        this->spk_->play(buf, static_cast<size_t>(len));
      }
#endif
      this->last_activity_ms_ = now;
      this->state_ = BridgeState::RECEIVING;
    } else if (len < 0) {
      ESP_LOGE(TAG, "WebSocket read error — stopping");
      this->stop_session();
    }
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

int ARIABridge::ws_recv_frame_(uint8_t *buf, size_t max_len) {
  if (this->sock_fd_ < 0) return -1;

  uint8_t header[2];
  int r = lwip_recv(this->sock_fd_, header, 2, MSG_PEEK | MSG_DONTWAIT);
  if (r <= 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
  }
  if (r < 2) return 0;

  lwip_recv(this->sock_fd_, header, 2, MSG_DONTWAIT);

  uint8_t opcode = header[0] & 0x0F;
  bool masked = (header[1] & 0x80) != 0;
  size_t payload_len = header[1] & 0x7F;

  if (payload_len == 126) {
    uint8_t ext[2];
    if (lwip_recv(this->sock_fd_, ext, 2, MSG_DONTWAIT) != 2)
      return -1;
    payload_len = (ext[0] << 8) | ext[1];
  } else if (payload_len == 127) {
    uint8_t ext[8];
    lwip_recv(this->sock_fd_, ext, 8, MSG_DONTWAIT);
    return 0;
  }

  uint8_t mask_key[4] = {0};
  if (masked) {
    if (lwip_recv(this->sock_fd_, mask_key, 4, MSG_DONTWAIT) != 4)
      return -1;
  }

  if (payload_len > max_len) {
    uint8_t discard[256];
    size_t remaining = payload_len;
    while (remaining > 0) {
      size_t chunk = remaining > sizeof(discard) ? sizeof(discard) : remaining;
      int rd = lwip_recv(this->sock_fd_, discard, chunk, MSG_DONTWAIT);
      if (rd <= 0) return -1;
      remaining -= rd;
    }
    return 0;
  }

  size_t total = 0;
  while (total < payload_len) {
    r = lwip_recv(this->sock_fd_, buf + total, payload_len - total, MSG_DONTWAIT);
    if (r <= 0) {
      if ((errno == EAGAIN || errno == EWOULDBLOCK) && total > 0)
        continue;
      return (total > 0) ? -1 : 0;
    }
    total += r;
  }

  if (masked) {
    for (size_t i = 0; i < total; i++)
      buf[i] ^= mask_key[i % 4];
  }

  if (opcode == 0x08) {
    ESP_LOGI(TAG, "Received WS close frame");
    return -1;
  }

  if (opcode == 0x09) {
    uint8_t pong[6] = {0x8A, 0x80, 0x00, 0x00, 0x00, 0x00};
    lwip_send(this->sock_fd_, pong, sizeof(pong), 0);
    return 0;
  }

  if (opcode == 0x01) {
    std::string msg(reinterpret_cast<char *>(buf), total);
    ESP_LOGI(TAG, "Bridge status: %s", msg.c_str());
    if (msg.find("\"done\"") != std::string::npos) {
      this->state_ = BridgeState::STREAMING;  // ARIA finished — resume mic
    } else if (msg.find("\"processing\"") != std::string::npos) {
      this->state_ = BridgeState::RECEIVING;  // v12: pause mic, listen for TTS (half-duplex)
    } else if (msg.find("\"timeout\"") != std::string::npos) {
      return -1;
    }
    return 0;
  }

  if (opcode == 0x02) {
    return static_cast<int>(total);
  }

  return 0;
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
      self->last_activity_ms_ = millis();
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
  this->state_ = BridgeState::CONNECTING;
  this->session_start_ms_ = millis();
  this->last_activity_ms_ = millis();
  this->mic_dropped_bytes_ = 0;  // v11: reset per-session stats
  this->bytes_sent_ = 0;

  {
    std::lock_guard<std::mutex> lock(this->mic_mutex_);
    this->mic_buffer_.clear();
  }

#ifdef USE_SPEAKER
  if (this->spk_ != nullptr) {
    this->spk_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, 48000));  // bridge now sends 48kHz (matches I2S speaker)
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
  this->state_ = BridgeState::IDLE;
}

}  // namespace aria_bridge
}  // namespace esphome
