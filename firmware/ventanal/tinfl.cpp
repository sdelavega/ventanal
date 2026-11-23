// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Stephen de la Vega
// The vendored miniz inflater (shared with the LCD board) for zlib e-paper
// frames. PlatformIO builds only *.cpp from this directory, so the C source
// is compiled here as C++; its declarations keep C linkage.
#include "../lcd43b/src/miniz/miniz_tinfl.c"
