// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega

#include <xtensa/config/core.h>

    // One aligned save area per call on the internal task or interrupt stack.
    // Leave room for window spills on both sides of the CP3 save area.
    .equ .Lcp3Frame, 16 + XCHAL_CP3_SA_SIZE + 16
    .if XCHAL_CP3_SA_ALIGN != 16 || (.Lcp3Frame & 15)
    .error "Unexpected CP3 stack alignment"
    .endif

    .section .iram1,"ax"
    .align  4
    .global lcdDimRgb565Vectors
    .type   lcdDimRgb565Vectors,@function

// a2 = 16-byte aligned pixels, a3 = number of eight-pixel blocks,
// a4 = brightness quarters (1..3), a5 = four aligned vectors of masks.
lcdDimRgb565Vectors:
    entry   sp, .Lcp3Frame
    // The RTOS can leave the PIE coprocessor disabled in interrupt context.
    // Enable CP3 for this routine and restore the entry CPENABLE on exit.
    rsr     a11, cpenable
    movi    a12, 8
    or      a12, a11, a12
    wsr     a12, cpenable
    rsync
    rsr     a7, sar
    addi    a6, sp, 16
    xchal_cp3_store a6, a8, a9, a10, a12

    ee.vld.128.ip q4, a5, 16 // Mask for floor(channel / 2).
    ee.vld.128.ip q5, a5, 16 // Mask for floor(channel / 4).
    mov     a9, a2
    beqi    a4, 3, .Lthree
    beqi    a4, 2, .Lhalf

    movi    a8, 2
    ssr     a8
    loopgtz a3, .Lquarter_done
    ee.vld.128.ip q0, a2, 16
    ee.vsr.32 q0, q0
    ee.andq q0, q0, q5
    ee.vst.128.ip q0, a9, 16
.Lquarter_done:
    j       .Lrestore

.Lhalf:
    movi    a8, 1
    ssr     a8
    loopgtz a3, .Lhalf_done
    ee.vld.128.ip q0, a2, 16
    ee.vsr.32 q0, q0
    ee.andq q0, q0, q4
    ee.vst.128.ip q0, a9, 16
.Lhalf_done:
    j       .Lrestore

.Lthree:
    ee.vld.128.ip q3, a5, 16 // Low bit of each channel.
    ee.vld.128.ip q6, a5, 16 // Bias for signed vector addition.
    movi    a8, 1
    movi    a10, 2
    loopgtz a3, .Lrestore
    ee.vld.128.ip q0, a2, 16
    ssr     a8
    ee.vsr.32 q1, q0
    // floor(3*c/4) = floor(c/2) + floor(c/4) + (c % 4 == 3).
    ee.andq q2, q0, q1
    ee.andq q2, q2, q3
    ee.andq q1, q1, q4
    ssr     a10
    ee.vsr.32 q0, q0
    ee.andq q0, q0, q5
    // Bias the nonnegative half before summing to avoid signed saturation.
    ee.xorq q1, q1, q6
    ee.vadds.s16 q1, q1, q0
    ee.vadds.s16 q1, q1, q2
    ee.xorq q1, q1, q6
    ee.vst.128.ip q1, a9, 16

.Lrestore:
    addi    a6, sp, 16
    xchal_cp3_load a6, a8, a9, a10, a12
    wsr     a7, sar
    wsr     a11, cpenable
    rsync
    retw
    .size lcdDimRgb565Vectors, .-lcdDimRgb565Vectors
