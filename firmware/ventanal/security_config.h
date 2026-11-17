// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#ifdef VENTANAL_HOST_TEST
#include "config.example.h"
#else
#include "config.h"
#endif
// Diagnostic builds may override transport values without editing config.h.
#ifdef VENTANAL_SECURITY_CONFIG
#define VENTANAL_STRINGIFY_INNER(x) #x
#define VENTANAL_STRINGIFY(x) VENTANAL_STRINGIFY_INNER(x)
#include VENTANAL_STRINGIFY(VENTANAL_SECURITY_CONFIG)
#endif
#ifndef VENTANAL_HOST_CERT_PEM
#define VENTANAL_HOST_CERT_PEM ""
#endif
#ifndef VENTANAL_DEVICE_KEY
#define VENTANAL_DEVICE_KEY ""
#endif
