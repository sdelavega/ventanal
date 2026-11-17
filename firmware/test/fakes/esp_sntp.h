// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
constexpr int SNTP_SYNC_STATUS_COMPLETED = 1;
inline int fakeSyncStatus = SNTP_SYNC_STATUS_COMPLETED;
inline int sntp_get_sync_status() { int status = fakeSyncStatus; fakeSyncStatus = 0; return status; }
