#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/microphone/microphone.h"
#ifdef USE_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

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
#ifdef USE_SPEAKER
  void set_speaker(speaker::Speaker *spk) { this->spk_ = spk; }
#endif

  void start_session();
  void stop_session();
  bool is_active() const { return this->state_ != BridgeState::IDLE; }

 protected:
  static void connect_task_(void *param);
  static void send_task_(void *param);
  bool tcp_connect_();
  bool ws_handshake_();
  void ws_disconnect_();
  bool ws_send_binary_(const uint8_t *data, size_t len);
  int ws_recv_frame_(uint8_t *buf, size_t max_len);
  int recv_exact_(uint8_t *buf, size_t n, uint32_t timeout_ms);  // atomic read (no frame desync)
  bool parse_url_(std::string &host, uint16_t &port, std::string &path);

  std::string bridge_url_;
  int sample_rate_{16000};
  microphone::Microphone *mic_{nullptr};
#ifdef USE_SPEAKER
  speaker::Speaker *spk_{nullptr};
#endif
  std::atomic<int> sock_fd_{-1};
  std::atomic<BridgeState> state_{BridgeState::IDLE};
  std::atomic<uint32_t> last_activity_ms_{0};
  uint32_t session_start_ms_{0};
  std::atomic<TaskHandle_t> send_task_handle_{nullptr};
  std::atomic<bool> send_task_running_{false};
  static constexpr uint32_t SESSION_TIMEOUT_MS = 30000;
  static constexpr size_t MAX_MIC_BUFFER_BYTES = 65536;

  // v11: per-session comms stats (logged at stop_session)
  std::atomic<uint32_t> mic_dropped_bytes_{0};
  std::atomic<uint32_t> bytes_sent_{0};

  std::vector<uint8_t, ExternalRAMAllocator<uint8_t>> mic_buffer_;
  std::mutex mic_mutex_;
  std::vector<uint8_t> spk_pending_;  // v18: speaker bytes not yet accepted (backpressure, no drop)
};

}  // namespace aria_bridge
}  // namespace esphome
