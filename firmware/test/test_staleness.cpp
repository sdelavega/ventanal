// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#include "../common/staleness.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>

using ventanal::RefreshLabel;
using ventanal::labelFor;
using ventanal::weatherAgeHours;

constexpr uint32_t epoch = 1704067200;
constexpr uint32_t day = 24 * 60 * 60;

struct CheckpointStore {
    uint32_t data[2]{};
    bool failWrite = false;
    size_t writes = 0;

    size_t putBytes(const char*, const void* src, size_t size) {
        ++writes;
        if (failWrite) return 0;
        assert(size == sizeof data);
        memcpy(data, src, size);
        return size;
    }

    size_t getBytes(const char*, void* dst, size_t size) const {
        assert(size == sizeof data);
        memcpy(dst, data, size);
        return size;
    }
};

void art_daily_late_arrival_no_label() {
    ventanal::ContentStaleness content{};
    content.observe(epoch, true);
    content.clearOverdue(); // Yesterday's art arrived.
    content.observe(epoch, false);
    content.observe(epoch + day, true);
    assert(content.overdueSince == epoch + day);
    assert(content.label(false, epoch + day) == RefreshLabel::None);
}

void overdue_24h_shows_label() {
    ventanal::ContentStaleness content{};
    content.observe(epoch, false);
    const auto overdueAt = epoch + 600;
    content.observe(overdueAt, true);
    content.observe(overdueAt + 60, true);
    assert(content.label(false, overdueAt + day - 1) == RefreshLabel::None);
    assert(content.label(false, overdueAt + day) == RefreshLabel::Failed);
    assert(content.label(false, 0) == RefreshLabel::None);

    content.observe(overdueAt + day, false);
    content.observe(overdueAt + day + 60, true);
    assert(content.label(false, overdueAt + day + 60) == RefreshLabel::None);
}

void new_content_clears_overdue() {
    ventanal::ContentStaleness content{};
    content.observe(epoch, true);
    assert(content.label(false, epoch + day) == RefreshLabel::Failed);
    content.clearOverdue();
    assert(content.overdueSince == 0);
    assert(content.label(false, epoch + day) == RefreshLabel::None);
    content.observe(0, true);
    assert(content.overdueSince == 0);
    content.observe(epoch + day + 60, true);
    assert(content.label(false, epoch + 2 * day + 59) == RefreshLabel::None);
    assert(content.label(false, epoch + 2 * day + 60) == RefreshLabel::Failed);
}

void overdue_survives_reboot_via_checkpoint() {
    CheckpointStore store;
    ventanal::ContentStaleness content{};
    content.observe(epoch, true);
    assert(content.checkpointDue(epoch));
    content.checkpoint(store, epoch);

    ventanal::ContentStaleness reboot{};
    reboot.restore(store);
    assert(reboot.due && reboot.overdueSince == epoch);
    reboot.observe(epoch + day - 60, true);
    assert(reboot.label(false, epoch + day - 1) == RefreshLabel::None);
    assert(reboot.label(false, epoch + day) == RefreshLabel::Failed);
}

void manual_refresh_label_before_clock() {
    ventanal::ContentStaleness content{};
    content.observe(epoch - 1, true);
    assert(content.label(true, epoch - 1) == RefreshLabel::Refreshing);
    assert(content.label(false, epoch - 1) == RefreshLabel::None);
    content.observe(epoch, true);
    assert(content.label(false, epoch + day - 1) == RefreshLabel::None);
    assert(content.label(false, epoch + day) == RefreshLabel::Failed);
}

void checkpoint_retry_after_failed_write() {
    CheckpointStore store;
    ventanal::ContentStaleness content{};
    content.observe(epoch, true);
    assert(content.checkpointDue(epoch));
    store.failWrite = true;
    content.checkpoint(store, epoch);
    assert(content.checkpointDue(epoch + 60));
    store.failWrite = false;
    content.checkpoint(store, epoch + 60);
    assert(store.writes == 2);
    assert(!content.checkpointDue(epoch + 120));
}

void transient_failure_shows_nothing() {
    assert(labelFor(false, epoch, epoch + 60) == RefreshLabel::None);
    assert(labelFor(true, epoch, epoch + 60) == RefreshLabel::Refreshing);
}

void failure_after_24h() {
    assert(labelFor(false, epoch, epoch + day - 1) == RefreshLabel::None);
    assert(labelFor(false, epoch, epoch + day) == RefreshLabel::Failed);
    assert(labelFor(false, 0, epoch + day) == RefreshLabel::None);
    assert(labelFor(false, epoch + 1, epoch) == RefreshLabel::None);
}

void failure_and_weather_age_need_valid_time() {
    assert(labelFor(false, epoch, epoch - 1) == RefreshLabel::None);
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
    CheckpointStore store;
    ventanal::ContentStaleness live{};
    live.restore(store);
    live.observe(0, true);
    assert(!live.checkpointDue(0));
    live.observe(epoch, true);
    assert(live.checkpointDue(epoch));
    live.checkpoint(store, epoch);
    live.observe(epoch + 60, false);
    live.restore(store); // A later wake keeps the RTC state ahead of the checkpoint.
    assert(!live.due && !live.overdueSince);
    assert(!live.checkpointDue(epoch + day - 1));
    assert(live.checkpointDue(epoch + day));
    live.checkpoint(store, epoch + day);
    assert(store.data[0] == 0);
    assert(store.data[1] == epoch + day);
    ventanal::ContentStaleness reboot{};
    reboot.restore(store);
    assert(!reboot.due);
    assert(reboot.label(false, epoch + day) == RefreshLabel::None);
    reboot.observe(epoch + day + 60, true);
    assert(!reboot.checkpointDue(epoch + 2 * day - 1));
    assert(reboot.checkpointDue(epoch + 2 * day));
}

int main() {
    art_daily_late_arrival_no_label();
    overdue_24h_shows_label();
    new_content_clears_overdue();
    overdue_survives_reboot_via_checkpoint();
    manual_refresh_label_before_clock();
    checkpoint_retry_after_failed_write();
    transient_failure_shows_nothing();
    failure_after_24h();
    failure_and_weather_age_need_valid_time();
    weather_age_floor_hours();
    checkpoint_rate_and_restore();
    puts("staleness: refresh labels, weather age and checkpoints passed");
}
