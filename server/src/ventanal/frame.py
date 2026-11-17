# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

"""Encode and decode the fixed-size VNF1 six-colour panel format."""

import hashlib
import struct

import numpy as np


FRAME_W, FRAME_H = 1600, 1200
FRAME_SIZE = 960016
_HEADER = struct.pack("<4sHH8x", b"VNF1", FRAME_W, FRAME_H)


def encode_frame(idx: np.ndarray) -> bytes:
    """Pack row-major uint8 indices with the left pixel in the high nibble."""
    if idx.shape != (FRAME_H, FRAME_W) or idx.dtype != np.uint8:
        raise ValueError("indices must be a 1200 by 1600 uint8 array")
    if np.any(idx > 5):
        raise ValueError("panel indices must be in the range 0–5")
    packed = (idx[:, 0::2] << 4) | idx[:, 1::2]
    return _HEADER + packed.tobytes()


def decode_frame(data: bytes) -> np.ndarray:
    """Validate a VNF1 frame and return its uint8 panel indices."""
    if len(data) != FRAME_SIZE or data[:16] != _HEADER:
        raise ValueError("invalid VNF1 frame size or header")
    packed = np.frombuffer(data, dtype=np.uint8, offset=16).reshape(FRAME_H, FRAME_W // 2)
    idx = np.empty((FRAME_H, FRAME_W), dtype=np.uint8)
    idx[:, 0::2] = packed >> 4
    idx[:, 1::2] = packed & 0x0F
    if np.any(idx > 5):
        raise ValueError("panel indices must be in the range 0–5")
    return idx


def frame_id(data: bytes) -> str:
    """Identify encoded content by the first 16 hexadecimal SHA-256 digits."""
    return hashlib.sha256(data).hexdigest()[:16]
