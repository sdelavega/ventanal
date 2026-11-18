// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Use ESP32-S3's 32-bit bit buffer and portable byte loads on both targets.
#define MINIZ_HAS_64BIT_REGISTERS 0
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME

// The ESP32-S3 linker script assigns tinfl names to mask-ROM addresses.
#define tinfl_decompress ventanal_tinfl_decompress
#define tinfl_decompress_mem_to_heap ventanal_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem ventanal_tinfl_decompress_mem_to_mem
#define tinfl_decompress_mem_to_callback ventanal_tinfl_decompress_mem_to_callback
#define tinfl_decompressor_alloc ventanal_tinfl_decompressor_alloc
#define tinfl_decompressor_free ventanal_tinfl_decompressor_free

#include "miniz_tinfl.h"
