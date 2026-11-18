#pragma once
#include <cstdio>
#define ESPHOME_LOG_TAG(name, tag) static constexpr const char *const name = tag
#define HOST_LOG_QUIET(tag, ...) \
  do { \
    if (0) { \
      printf(__VA_ARGS__); \
    } \
  } while (0)
#define HOST_LOG_SHOWN(level, tag, ...) \
  do { \
    printf("    [" level "][%s] ", tag); \
    printf(__VA_ARGS__); \
    printf("\n"); \
  } while (0)
#define ESP_LOGE(tag, ...) HOST_LOG_SHOWN("E", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) HOST_LOG_SHOWN("W", tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) HOST_LOG_QUIET(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) HOST_LOG_QUIET(tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) HOST_LOG_QUIET(tag, __VA_ARGS__)
#define ESP_LOGCONFIG(tag, ...) HOST_LOG_QUIET(tag, __VA_ARGS__)
