// 主机模拟器垫片:FreeRTOS 极简类型与宏(单线程,队列用同步数组实现)。
// 仅用于 tools/sim 主机模拟,不参与固件构建。
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;   // 与 sim_dark.c 的 pthread 垫片配套
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  pdTRUE
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) (ms)
