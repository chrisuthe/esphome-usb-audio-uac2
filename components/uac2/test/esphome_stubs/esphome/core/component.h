#pragma once
// Stand-in for ESPHome's Component: just enough for the speaker test to drive loop() itself.
namespace esphome {

struct LogString;

namespace setup_priority {
inline constexpr float BUS = 1000.0f;
}  // namespace setup_priority

class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0.0f; }
  virtual bool teardown() { return true; }
  void enable_loop() { this->loop_enabled = true; }
  void disable_loop() { this->loop_enabled = false; }
  void enable_loop_soon_any_context() { this->loop_enabled = true; }
  bool loop_enabled{true};
};

}  // namespace esphome
