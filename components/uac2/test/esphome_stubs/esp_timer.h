#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// The test's clock, in microseconds
int64_t esp_timer_get_time(void);
#ifdef __cplusplus
}
#endif
