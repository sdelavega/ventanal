// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#include "FreeRTOS.h"
#include <cassert>
using TaskHandle_t = void (*)(void*);
inline TaskHandle_t notifiedTask=nullptr;
inline unsigned taskStackBytes=0;
inline int xTaskCreate(TaskHandle_t task,const char*,unsigned stack,void*,unsigned,TaskHandle_t* handle) {
    *handle=task; taskStackBytes=stack; return pdPASS;
}
inline unsigned uxTaskPriorityGet(TaskHandle_t) { return 1; }
inline void xTaskNotifyGive(TaskHandle_t task) { assert(task && !notifiedTask); notifiedTask=task; }
inline unsigned ulTaskNotifyTake(int,unsigned) { assert(notifiedTask); notifiedTask=nullptr; return 1; }
inline void vTaskDelete(TaskHandle_t) {}
inline void dispatchNotifiedTask() { if(notifiedTask) notifiedTask(nullptr); }
