#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/automation.h"
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
#include <functional>

namespace esphome {
namespace aria_bridge {

enum class BridgeState : uint8_t {
  IDLE,
  CONNECTING,
  STREAMING,
  RECEIVING,
  ERROR,
};

// v21: LED phase machine — drives the on_*_start / on_idle / on_error triggers (YAML wires
// each to a light effect). Distinct from BridgeState so wake-fires-immediately can be modeled
// without coupling to the WS state machine.
enum class LedPhase : uint8_t {
  IDLE,
  LISTENING,
  THINKING,
  RESPONDING,
  ERROR_PHASE,
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
  void start_session_with_uuid(const std::string &uuid);   // v23: caller-provided session_id (from YAML)
  void stop_session();
  bool is_active() const { return this->state_ != BridgeState::IDLE; }

  // v23: pre-wake audio audit substrate — PSRAM ring fed unconditionally by the mic callback.
  // Output uses ExternalRAMAllocator: a 256KB output buffer on the internal heap is too big
  // (causes bad_alloc → IllegalInstruction crash on ESP32-S3). PSRAM allocator avoids it.
  using PrewakeBuffer = std::vector<uint8_t, ExternalRAMAllocator<uint8_t>>;
  PrewakeBuffer snapshot_prewake();
  std::string current_session_uuid() const { return this->session_uuid_; }

  // v23: bridge event emit. Source is implicitly "satellite"; fire-and-forget HTTP POST queued
  // for the event_post_task_. payload_json is the inner JSON object (e.g. {"k":"v"}) — emit
  // builds the full record envelope around it.
  void emit_event(const std::string &event_type, const std::string &payload_json);

  void set_event_post_url(const std::string &url) { this->event_post_url_ = url; }   // v23: from codegen
  void set_prewake_post_url(const std::string &url) { this->prewake_post_url_ = url; }
  void set_prewake_seconds(int s) { this->prewake_seconds_ = s; }
  void set_satellite_id(const std::string &id) { this->satellite_id_ = id; }

  // v21: LED state-machine subscription API (used by the per-trigger glue classes below).
  void add_on_listening_start_callback(std::function<void()> &&cb) { this->on_listening_start_cb_.add(std::move(cb)); }
  void add_on_thinking_start_callback(std::function<void()> &&cb)  { this->on_thinking_start_cb_.add(std::move(cb)); }
  void add_on_responding_start_callback(std::function<void()> &&cb){ this->on_responding_start_cb_.add(std::move(cb)); }
  void add_on_error_callback(std::function<void()> &&cb)           { this->on_error_cb_.add(std::move(cb)); }
  void add_on_idle_callback(std::function<void()> &&cb)            { this->on_idle_cb_.add(std::move(cb)); }

 protected:
  // v21: fire-on-entry helpers — call the registered automations only when the phase changes.
  void fire_listening_();
  void fire_thinking_();
  void fire_responding_();
  void fire_error_();
  void fire_idle_();

  static void connect_task_(void *param);
  static void send_task_(void *param);
  bool tcp_connect_();
  bool ws_handshake_();
  void ws_disconnect_();
  bool ws_send_binary_(const uint8_t *data, size_t len);
  bool ws_send_text_(const char *data, size_t len);              // v20: send text frame (playback_complete)
  void send_playback_complete_();                                // v20: tell bridge speaker drained
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

  // v20: playback-complete signal — emitted when the i2s buffer has fully drained, so the
  // bridge can keep its half-duplex gate closed until the speaker is actually silent (response.done
  // fires when Grok finishes SENDING audio, before the satellite has finished PLAYING it).
  std::atomic<uint32_t> last_spk_play_ms_{0};       // millis() of last successful spk_->play() w/ bytes accepted
  std::atomic<bool> playback_complete_sent_{false}; // ensure exactly one signal per playback session
  static constexpr uint32_t PLAYBACK_IDLE_MS = 700; // 500ms i2s buffer drain + 200ms acoustic margin

  // v21: LED phase machine + callback fan-out (one CallbackManager per phase entry).
  std::atomic<LedPhase> led_phase_{LedPhase::IDLE};
  CallbackManager<void()> on_listening_start_cb_;
  CallbackManager<void()> on_thinking_start_cb_;
  CallbackManager<void()> on_responding_start_cb_;
  CallbackManager<void()> on_error_cb_;
  CallbackManager<void()> on_idle_cb_;

  // v23: pre-wake audio ring (PSRAM). The mic callback writes here UNCONDITIONALLY (not gated
  // by session state) so that on start_session we can flush the last N seconds of audio that
  // led up to the wake-word firing — the audit substrate for tuning the wake pipeline.
  std::vector<uint8_t, ExternalRAMAllocator<uint8_t>> prewake_ring_;
  size_t prewake_head_{0};
  bool prewake_full_{false};
  std::mutex prewake_mutex_;
  int prewake_seconds_{2};

  std::string session_uuid_;
  std::string event_post_url_;     // "http://192.168.51.179:8766/satellite-event"
  std::string prewake_post_url_;   // "http://192.168.51.179:8766/satellite-prewake-audio"
  std::string satellite_id_{"satellite1-kis"};

  // v23: outgoing event queue + worker task — drained over an HTTP POST per event,
  // fire-and-forget. Independent of the session WS so events flow even when no session.
  struct OutgoingEvent { std::string event; std::string payload_json; std::string session_id; };
  std::vector<OutgoingEvent> event_queue_;
  std::mutex event_queue_mutex_;
  std::atomic<TaskHandle_t> event_post_task_handle_{nullptr};
  std::atomic<bool> event_post_task_running_{false};

  // v23: helpers
  void prewake_ring_write_(const uint8_t *data, size_t len);
  static void event_post_task_(void *param);
  static void prewake_upload_task_(void *param);                        // dispatched per-session
  bool http_post_json_(const std::string &path_with_qs, const std::string &body) const;
  bool http_post_binary_(const std::string &path_with_qs, const uint8_t *body, size_t len) const;
};

// v21: ESPHome trigger glue — each instance subscribes to the matching ARIABridge callback and
// fires the Trigger<> when the phase entered. YAML attaches automations via on_listening_start etc.
class ListeningStartTrigger : public Trigger<> {
 public:
  explicit ListeningStartTrigger(ARIABridge *parent) {
    parent->add_on_listening_start_callback([this]() { this->trigger(); });
  }
};
class ThinkingStartTrigger : public Trigger<> {
 public:
  explicit ThinkingStartTrigger(ARIABridge *parent) {
    parent->add_on_thinking_start_callback([this]() { this->trigger(); });
  }
};
class RespondingStartTrigger : public Trigger<> {
 public:
  explicit RespondingStartTrigger(ARIABridge *parent) {
    parent->add_on_responding_start_callback([this]() { this->trigger(); });
  }
};
class ErrorTrigger : public Trigger<> {
 public:
  explicit ErrorTrigger(ARIABridge *parent) {
    parent->add_on_error_callback([this]() { this->trigger(); });
  }
};
class IdleTrigger : public Trigger<> {
 public:
  explicit IdleTrigger(ARIABridge *parent) {
    parent->add_on_idle_callback([this]() { this->trigger(); });
  }
};

}  // namespace aria_bridge
}  // namespace esphome
