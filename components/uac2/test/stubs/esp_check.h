#pragma once
#include "esp_err.h"
#include "esp_log.h"
#define unlikely(x) __builtin_expect(!!(x), 0)
#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...) do { esp_err_t err_rc_ = (x); if (err_rc_ != ESP_OK) { ESP_LOGE(tag, fmt, ##__VA_ARGS__); return err_rc_; } } while (0)
#define ESP_RETURN_ON_FALSE(a, err, tag, fmt, ...) do { if (!(a)) { ESP_LOGE(tag, fmt, ##__VA_ARGS__); return err; } } while (0)
#define ESP_GOTO_ON_ERROR(x, lbl, tag, fmt, ...) do { esp_err_t err_rc_ = (x); if (err_rc_ != ESP_OK) { ESP_LOGE(tag, fmt, ##__VA_ARGS__); ret = err_rc_; goto lbl; } } while (0)
#define ESP_GOTO_ON_FALSE(a, err, lbl, tag, fmt, ...) do { if (!(a)) { ESP_LOGE(tag, fmt, ##__VA_ARGS__); ret = err; goto lbl; } } while (0)
