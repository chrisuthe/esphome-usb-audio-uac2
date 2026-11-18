#pragma once
// Stand-in for ESPHome's speaker::Speaker with the members the USB speaker uses.
#include <cstddef>
#include <cstdint>
#include <utility>

#include "esphome/components/audio/audio.h"
#include "esphome/core/helpers.h"

extern "C" {
#include "freertos/FreeRTOS.h"
}

namespace esphome::speaker {

enum State : uint8_t {
  STATE_STOPPED = 0,
  STATE_STARTING,
  STATE_RUNNING,
  STATE_STOPPING,
};

class Speaker {
 public:
  virtual ~Speaker() = default;
  virtual size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) { return this->play(data, length); }
  virtual size_t play(const uint8_t *data, size_t length) = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual void finish() { this->stop(); }
  virtual bool has_buffered_data() const = 0;
  bool is_running() const { return this->state_ == STATE_RUNNING; }
  bool is_stopped() const { return this->state_ == STATE_STOPPED; }
  virtual void set_volume(float volume) { this->volume_ = volume; }
  virtual void set_mute_state(bool mute_state) { this->mute_state_ = mute_state; }
  template<typename F> void add_audio_output_callback(F &&callback) {
    this->audio_output_callback_.add(std::forward<F>(callback));
  }

 protected:
  State state_{STATE_STOPPED};
  bool mute_state_{false};
  audio::AudioStreamInfo audio_stream_info_;
  float volume_{1.0f};
  CallbackManager<void(uint32_t, int64_t)> audio_output_callback_{};
};

}  // namespace esphome::speaker
