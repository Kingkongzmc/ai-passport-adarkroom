#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, ...) do { fprintf(stderr, "E %s: ", tag); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define ESP_LOGW(tag, ...) do { fprintf(stderr, "W %s: ", tag); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define ESP_LOGI(tag, ...) do { fprintf(stderr, "I %s: ", tag); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
