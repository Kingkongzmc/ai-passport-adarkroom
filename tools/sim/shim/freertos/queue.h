#pragma once
#include "FreeRTOS.h"

// 主机模拟用队列:固定容量环形缓冲,满则丢弃(与固件 xQueueSend(timeout=0) 语义一致)。
typedef struct sim_queue *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *out, TickType_t wait);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);
