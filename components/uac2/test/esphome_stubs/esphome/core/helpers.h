#pragma once
#include <functional>
#include <utility>
#include <vector>

namespace esphome {

template<typename T> class Parented {
 public:
  void set_parent(T *parent) { this->parent_ = parent; }

 protected:
  T *parent_{nullptr};
};

template<typename... X> class CallbackManager;
template<typename... Ts> class CallbackManager<void(Ts...)> {
 public:
  void add(std::function<void(Ts...)> &&callback) { this->callbacks_.push_back(std::move(callback)); }
  void operator()(Ts... args) {
    for (auto &callback : this->callbacks_) {
      callback(args...);
    }
  }

 protected:
  std::vector<std::function<void(Ts...)>> callbacks_;
};

}  // namespace esphome
