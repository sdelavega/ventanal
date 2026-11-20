// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once

#include <ArduinoJson.h>
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace lcd::local {

inline JsonDocument localJsonDocument() {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
    class PsramAllocator final : public ArduinoJson::Allocator {
        void* allocate(size_t size) override {
            return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        void deallocate(void* ptr) override { heap_caps_free(ptr); }
        void* reallocate(void* ptr, size_t size) override {
            return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    };
    static PsramAllocator allocator;
    return JsonDocument(&allocator);
#else
    return JsonDocument();
#endif
}

}  // namespace lcd::local
