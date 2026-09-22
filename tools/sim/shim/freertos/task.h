#pragma once
// 模拟器垫片:任务 API 由 sim_dark.c 用 pthread 实现
typedef void (*sim_task_fn_t)(void *);
BaseType_t xTaskCreate(sim_task_fn_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t prio, void **handle);
void vTaskDelete(void *t);
void vTaskDelay(TickType_t ms);
