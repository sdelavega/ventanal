// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../common/staleness.h"

#include <cassert>
#include <cstddef>
#include <cstring>

using ventanal::RefreshLabel;
using ventanal::labelFor;
using ventanal::weatherAgeHours;

constexpr uint32_t epoch = 1704067200;
constexpr uint32_t day = 24 * 60 * 60;

void transient_failure_shows_nothing() {
    assert(labelFor(false, false, epoch, epoch + 60) == RefreshLabel::None);
    assert(labelFor(true, false, epoch, epoch + 60) == RefreshLabel::Refreshing);
}

void failure_after_24h() {
    assert(labelFor(false, false, epoch, epoch + day - 1) == RefreshLabel::None);
    assert(labelFor(false, false, epoch, epoch + day) == RefreshLabel::Failed);
    assert(labelFor(false, true, epoch, epoch) == RefreshLabel::Failed);
    assert(labelFor(false, false, 0, epoch + day) == RefreshLabel::None);
    assert(labelFor(false, false, epoch + 1, epoch) == RefreshLabel::None);
}

void labels_need_valid_time() {
    assert(labelFor(true, true, epoch - day - 1, epoch - 1) == RefreshLabel::None);
    assert(weatherAgeHours(epoch - 3601, epoch - 1) == -1);
}

void weather_age_floor_hours() {
    assert(weatherAgeHours(epoch, epoch + 59 * 60) == -1);
    assert(weatherAgeHours(epoch, epoch + 60 * 60) == 1);
    assert(weatherAgeHours(epoch, epoch + 61 * 60) == 1);
    assert(weatherAgeHours(epoch, epoch + 179 * 60) == 2);
    assert(weatherAgeHours(0, epoch) == -1);
    assert(weatherAgeHours(epoch + 1, epoch) == -1);
}

void checkpoint_rate_and_restore() {
    struct Store {
        uint32_t data[2]{};
        size_t putBytes(const char*, const void* src, size_t size) {
            assert(size == sizeof data);
            memcpy(data, src, size);
            return size;
        }
        size_t getBytes(const char*, void* dst, size_t size) const {
            assert(size == sizeof data);
            memcpy(dst, data, size);
            return size;
        }
    } store;
    ventanal::ContentStaleness live{};
    live.due = true;
    live.observe(0, true);
    assert(!live.checkpointDue(0));
    live.observe(epoch);
    assert(live.checkpointDue(epoch));
    live.checkpoint(store, epoch);
    live.observe(epoch + 60, true);
    assert(!live.checkpointDue(epoch + day - 1));
    assert(live.checkpointDue(epoch + day));
    assert(live.label(false, epoch + day) == RefreshLabel::None);
    assert(live.label(true, epoch + day + 60) == RefreshLabel::Refreshing);
    assert(live.label(false, epoch + day + 60) == RefreshLabel::Failed);
    live.checkpoint(store, epoch + day);
    assert(store.data[0] == epoch + 60);
    assert(store.data[1] == epoch + day);
    ventanal::ContentStaleness reboot{};
    reboot.restore(store);
    reboot.due = true;
    reboot.observe(epoch + 2 * day);
    assert(!reboot.checkpointDue(epoch + 2 * day));
    assert(reboot.label(false, epoch + 3 * day - 1) == RefreshLabel::None);
    assert(reboot.label(false, epoch + 3 * day) == RefreshLabel::Failed);
}

int main() {
    transient_failure_shows_nothing();
    failure_after_24h();
    labels_need_valid_time();
    weather_age_floor_hours();
    checkpoint_rate_and_restore();
}
