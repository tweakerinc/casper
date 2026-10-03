#pragma once
#include <esp_err.h>
inline int esp_task_wdt_status(void*) {return ESP_OK;}
inline int esp_task_wdt_reset() {return ESP_OK;}
