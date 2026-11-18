#pragma once
#include <stdio.h>
#include "esp_err.h"
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
#define HOST_LOG(l, tag, fmt, ...) printf("    [" l "][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) HOST_LOG("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HOST_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) HOST_LOG("I", tag, fmt, ##__VA_ARGS__)
// A level chosen at run time: printed down to INFO, like the lines above
#define ESP_LOG_LEVEL_LOCAL(level, tag, fmt, ...) do { if ((level) <= ESP_LOG_INFO) printf("    [%c][%s] " fmt "\n", (level) == ESP_LOG_ERROR ? 'E' : (level) == ESP_LOG_WARN ? 'W' : 'I', tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { if (0) printf(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGV(tag, fmt, ...) do { if (0) printf(fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOG_BUFFER_HEXDUMP(tag, buf, len, level) do { (void)(buf); } while (0)
#define ESP_LOG_BUFFER_HEX_LEVEL(tag, buf, len, level) do { (void)(buf); } while (0)
