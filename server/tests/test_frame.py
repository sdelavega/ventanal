# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega

import hashlib
import struct

import numpy as np
import pytest

from ventanal.frame import FRAME_H, FRAME_SIZE, FRAME_W, decode_frame, encode_frame, frame_id


def test_header_and_size():
    data = encode_frame(np.zeros((1200, 1600), dtype=np.uint8))
    assert (FRAME_W, FRAME_H, FRAME_SIZE) == (1600, 1200, 960016)
    assert len(data) == 960016
    assert data[:4] == b"VNF1"
    assert struct.unpack("<HH", data[4:8]) == (1600, 1200)
    assert data[8:16] == bytes(8)


def test_nibble_order():
    idx = np.zeros((1200, 1600), dtype=np.uint8)
    idx[0, 0], idx[0, 1] = 3, 5
    idx[1, 0], idx[1, 1] = 2, 4
    data = encode_frame(idx)
    assert data[16] == 0x35
    assert data[816] == 0x24


def test_roundtrip():
    idx = np.random.default_rng(42).integers(0, 6, (1200, 1600), dtype=np.uint8)
    result = decode_frame(encode_frame(idx))
    assert result.dtype == np.uint8
    np.testing.assert_array_equal(result, idx)


def test_frame_id():
    data = b"VNF1 example"
    assert frame_id(data) == hashlib.sha256(data).hexdigest()[:16]


@pytest.mark.parametrize("idx", [
    np.zeros((2, 2), dtype=np.uint8),
    np.zeros((1200, 1600), dtype=float),
    np.full((1200, 1600), 6, dtype=np.uint8),
])
def test_encode_rejects_invalid_indices(idx):
    with pytest.raises(ValueError):
        encode_frame(idx)


@pytest.mark.parametrize("offset,value", [(0, 0), (4, 0), (6, 0), (8, 1), (16, 0x60), (16, 0x06)])
def test_decode_rejects_invalid_frame(offset, value):
    data = bytearray(encode_frame(np.zeros((1200, 1600), dtype=np.uint8)))
    data[offset] = value
    with pytest.raises(ValueError):
        decode_frame(bytes(data))


@pytest.mark.parametrize("size", [0, 960015, 960017])
def test_decode_rejects_wrong_size(size):
    with pytest.raises(ValueError):
        decode_frame(bytes(size))
