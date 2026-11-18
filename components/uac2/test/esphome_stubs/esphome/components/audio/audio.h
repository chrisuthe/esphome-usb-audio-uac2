#pragma once
#include <cstddef>
#include <cstdint>

namespace esphome::audio {

class AudioStreamInfo {
 public:
  AudioStreamInfo() : AudioStreamInfo(16, 1, 16000) {}
  AudioStreamInfo(uint8_t bits_per_sample, uint8_t channels, uint32_t sample_rate)
      : sample_rate_(sample_rate), bits_per_sample_(bits_per_sample), channels_(channels) {}

  uint8_t get_bits_per_sample() const { return this->bits_per_sample_; }
  uint8_t get_channels() const { return this->channels_; }
  uint32_t get_sample_rate() const { return this->sample_rate_; }
  uint32_t bytes_to_frames(size_t bytes) const { return bytes / (this->bytes_per_sample_() * this->channels_); }
  size_t frames_to_bytes(uint32_t frames) const { return frames * this->bytes_per_sample_() * this->channels_; }
  bool operator==(const AudioStreamInfo &rhs) const {
    return this->sample_rate_ == rhs.sample_rate_ && this->bits_per_sample_ == rhs.bits_per_sample_ &&
           this->channels_ == rhs.channels_;
  }
  bool operator!=(const AudioStreamInfo &rhs) const { return !(*this == rhs); }

 protected:
  size_t bytes_per_sample_() const { return (this->bits_per_sample_ + 7) / 8; }
  uint32_t sample_rate_;
  uint8_t bits_per_sample_;
  uint8_t channels_;
};

}  // namespace esphome::audio
