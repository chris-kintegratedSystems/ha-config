#include "aria_bridge.h"
#include "esphome/core/log.h"
#include <cstring>

namespace esphome {
namespace aria_bridge {

static const char *const TAG = "aria_bridge";

void ARIABridge::setup() {
  ESP_LOGI(TAG, "ARIA Bridge initialized — URL: %s", this->bridge_url_.c_str());
}

void ARIABridge::loop() {
  if (this->state_ == BridgeState::IDLE)
    return;

  uint32_t now = millis();

  // Session timeout check
  if (now - this->last_activity_ms_ > SESSION_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Session timeout — stopping");
    this->stop_session();
    return;
  }

  // Stream microphone audio while in STREAMING state
  if (this->state_ == BridgeState::STREAMING && this->ws_client_ != nullptr) {
    if (esp_websocket_client_is_connected(this->ws_client_)) {
      std::vector<int16_t> samples(AUDIO_CHUNK_SAMPLES);
      // Read from microphone — ESPHome mic component provides samples
      size_t bytes_read = this->mic_->read(
          reinterpret_cast<int8_t *>(samples.data()),
          AUDIO_CHUNK_SAMPLES * sizeof(int16_t));
      if (bytes_read > 0) {
        samples.resize(bytes_read / sizeof(int16_t));
        this->send_audio_chunk_(samples);
        this->last_activity_ms_ = now;
      }
    }
  }
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

  // Start microphone
  this->mic_->start();

  // Connect WebSocket
  this->connect_ws_();
}

void ARIABridge::stop_session() {
  ESP_LOGI(TAG, "Stopping ARIA session (duration: %lums)",
           (unsigned long)(millis() - this->session_start_ms_));
  this->disconnect_ws_();
  this->mic_->stop();
  this->state_ = BridgeState::IDLE;
}

void ARIABridge::connect_ws_() {
  esp_websocket_client_config_t ws_cfg = {};
  ws_cfg.uri = this->bridge_url_.c_str();
  ws_cfg.buffer_size = 4096;
  ws_cfg.skip_cert_common_name_check = true;  // Self-signed cert on LAN

  this->ws_client_ = esp_websocket_client_init(&ws_cfg);
  if (this->ws_client_ == nullptr) {
    ESP_LOGE(TAG, "Failed to init WebSocket client");
    this->state_ = BridgeState::ERROR;
    return;
  }

  esp_websocket_register_events(this->ws_client_, WEBSOCKET_EVENT_ANY,
                                 ws_event_handler_, this);
  esp_err_t err = esp_websocket_client_start(this->ws_client_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start WebSocket: %s", esp_err_to_name(err));
    this->state_ = BridgeState::ERROR;
    return;
  }
  ESP_LOGI(TAG, "WebSocket connecting to %s", this->bridge_url_.c_str());
}

void ARIABridge::disconnect_ws_() {
  if (this->ws_client_ != nullptr) {
    esp_websocket_client_close(this->ws_client_, portMAX_DELAY);
    esp_websocket_client_destroy(this->ws_client_);
    this->ws_client_ = nullptr;
  }
}

void ARIABridge::send_audio_chunk_(const std::vector<int16_t> &samples) {
  if (this->ws_client_ == nullptr || !esp_websocket_client_is_connected(this->ws_client_))
    return;
  // Send raw 16-bit PCM as binary WebSocket frame
  esp_websocket_client_send_bin(
      this->ws_client_,
      reinterpret_cast<const char *>(samples.data()),
      samples.size() * sizeof(int16_t),
      portMAX_DELAY);
}

void ARIABridge::ws_event_handler_(void *arg, esp_event_base_t base,
                                    int32_t event_id, void *event_data) {
  auto *bridge = static_cast<ARIABridge *>(arg);
  auto *data = static_cast<esp_websocket_event_data_t *>(event_data);

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "WebSocket connected — streaming audio");
      bridge->state_ = BridgeState::STREAMING;
      bridge->last_activity_ms_ = millis();
      break;

    case WEBSOCKET_EVENT_DATA:
      bridge->last_activity_ms_ = millis();
      if (data->op_code == 0x02) {
        // Binary frame = audio response (16kHz PCM from bridge)
        bridge->spk_->start();
        bridge->spk_->write(reinterpret_cast<const uint8_t *>(data->data_ptr),
                            data->data_len);
        bridge->state_ = BridgeState::RECEIVING;
      } else if (data->op_code == 0x01) {
        // Text frame = JSON status from bridge
        std::string msg(data->data_ptr, data->data_len);
        ESP_LOGI(TAG, "Bridge status: %s", msg.c_str());
        if (msg.find("\"done\"") != std::string::npos) {
          // Response complete — ready for more speech
          bridge->state_ = BridgeState::STREAMING;
        } else if (msg.find("\"timeout\"") != std::string::npos) {
          bridge->stop_session();
        }
      }
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "WebSocket disconnected");
      bridge->stop_session();
      break;

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(TAG, "WebSocket error");
      bridge->state_ = BridgeState::ERROR;
      break;
  }
}

}  // namespace aria_bridge
}  // namespace esphome
