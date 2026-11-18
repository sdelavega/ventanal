// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
#pragma once
#define VENTANAL_STRINGIFY_INNER(x) #x
#define VENTANAL_STRINGIFY(x) VENTANAL_STRINGIFY_INNER(x)
#if defined(VENTANAL_INSTALLATION_CONFIG) && !defined(VENTANAL_HOST_TEST) && \
    !defined(VENTANAL_GENERIC_RELEASE)
#include VENTANAL_STRINGIFY(VENTANAL_INSTALLATION_CONFIG)
#else
#include "config.example.h"
#endif
// Diagnostic builds may override transport values without editing config.h.
#ifdef VENTANAL_SECURITY_CONFIG
#include VENTANAL_STRINGIFY(VENTANAL_SECURITY_CONFIG)
#endif
#ifndef VENTANAL_HOST_CERT_PEM
#define VENTANAL_HOST_CERT_PEM ""
#endif
#ifndef VENTANAL_DEVICE_KEY
#define VENTANAL_DEVICE_KEY ""
#endif
