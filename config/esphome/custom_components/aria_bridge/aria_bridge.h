#pragma once

#include "esphome/core/component.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include <esp_websocket_client.h>
#include <string>

namespace esphome {
namespace aria_bridge {

enum class BridgeState : uint8_t {
  IDLE,
  CONNECTING,
  STREAMING,
  RECEIVING,
  ERROR,
};

class ARIABridge : public Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_bridge_url(const std::string &url) { this->bridge_url_ = url; }
  void set_sample_rate(int rate) { this->sample_rate_ = rate; }
  void set_microphone(microphone::Microphone *mic) { this->mic_ = mic; }
  void set_speaker(speaker::Speaker *spk) { this->spk_ = spk; }

  /// Called by micro_wake_word on_wake_word_detected automation
  void start_session();
  void stop_session();
  bool is_active() const { return this->state_ != BridgeState::IDLE; }

 protected:
  void connect_ws_();
  void disconnect_ws_();
  void on_ws_event_(esp_websocket_event_data_t *event);
  void send_audio_chunk_(const std::vector<int16_t> &samples);

  static void ws_event_handler_(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *event_data);

  std::string bridge_url_;
  int sample_rate_{16000};
  microphone::Microphone *mic_{nullptr};
  speaker::Speaker *spk_{nullptr};
  esp_websocket_client_handle_t ws_client_{nullptr};
  BridgeState state_{BridgeState::IDLE};
  uint32_t last_activity_ms_{0};
  uint32_t session_start_ms_{0};
  static constexpr uint32_t SESSION_TIMEOUT_MS = 30000;
  static constexpr size_t AUDIO_CHUNK_SAMPLES = 512;  // 32ms at 16kHz
};

}  // namespace aria_bridge
}  // namespace esphome
