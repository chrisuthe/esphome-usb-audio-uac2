#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t f, const char *n, uint32_t s, void *a, UBaseType_t p, TaskHandle_t *h, BaseType_t c) { return pdTRUE; }
static inline void vTaskDelete(TaskHandle_t h) {}
#ifdef HOST_TEST_OWN_DELAY
// The test defines it, to move its clock and to let other "tasks" run while this one waits
void vTaskDelay(TickType_t t);
#else
static inline void vTaskDelay(TickType_t t) {}
#endif
static inline TickType_t xTaskGetTickCount(void) { return 0; }
static inline BaseType_t xTaskNotifyGive(TaskHandle_t h) { return pdTRUE; }
static inline uint32_t ulTaskNotifyTake(BaseType_t c, TickType_t t) { return 1; }
