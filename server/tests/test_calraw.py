# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Small real containers exercise the calibration decoders and linear math."""
from io import BytesIO
from pathlib import Path

import numpy as np
from PIL import Image, ImageCms
import pytest
import tifffile
import pillow_heif

from ventanal import calibrate
from ventanal.calibration_ui import CalibrationFiles

PROFILE = Path(__file__).parent/'fixtures/calraw/DisplayP3-v4.icc'


def dng(level, size=(64,64), orientation=1):
    data = BytesIO()
    # A real uncompressed Bayer DNG, generated here (no third-party photo).
    tags = [(50706, 'B', 4, (1,4,0,0), False),
            (50707, 'B', 4, (1,1,0,0), False),
            (50708, 's', 0, 'Ventanal synthetic camera', False),
            (33421, 'H', 2, (2,2), False),
            (33422, 'B', 4, (0,1,1,2), False),
            (50714, 'H', 1, 0, False), (50717, 'I', 1, 65535, False),
            (50721, '2i', 9, (1,1,0,1,0,1,0,1,1,1,0,1,0,1,0,1,1,1), False),
            (50778, 'H', 1, 21, False), (274, 'H', 1, orientation, False)]
    tifffile.imwrite(data, np.full(size,level,dtype=np.uint16),
                     photometric=32803, metadata=None, extratags=tags)
    return data.getvalue()


def test_real_dng_linear_exposure():
    from ventanal.calibration_photo import decode
    low, meta = decode(dng(4000), 'chart.dng')
    high, _ = decode(dng(8000), 'chart.dng')
    assert meta['raw'] is True and low.dtype == np.float32
    np.testing.assert_allclose(high[12:-12,12:-12]/low[12:-12,12:-12],2,atol=.02)


def test_linear_paper_controller_vector():
    from ventanal.calibration_photo import balance, encode
    samples = calibrate.srgb_to_linear([(26,26,37),(135,146,151),(150,146,59),
                                (69,28,27),(39,66,122),(58,84,74)])
    expected = [(42,39,46),(193,197,179),(214,197,71),(101,41,34),(60,91,145),(86,115,89)]
    np.testing.assert_allclose(encode(balance(samples,calibrate.srgb_to_linear((171,181,206)))),expected,atol=1)


@pytest.mark.parametrize('fmt,extension', [('PNG','png'),('JPEG','jpg'),('TIFF','tiff'),('HEIF','heic')])
def test_real_p3_containers(fmt,extension):
    from ventanal.calibration_photo import decode
    pillow_heif.register_heif_opener()
    photo = Image.new('RGB',(32,24),(180,90,80))
    profile = PROFILE.read_bytes()
    data = BytesIO(); photo.save(data,fmt,icc_profile=profile,quality=100)
    linear, meta = decode(data.getvalue(),'photo.'+extension)
    # Compare to independent LittleCMS transform, after actual codec decoding.
    with Image.open(BytesIO(data.getvalue())) as source:
        reference = ImageCms.profileToProfile(source.convert('RGB'),ImageCms.ImageCmsProfile(BytesIO(profile)),
                                              ImageCms.createProfile('sRGB'),outputMode='RGB')
    np.testing.assert_allclose(linear,calibrate.srgb_to_linear(reference),atol=1e-6)
    assert abs(float(linear[10,10,0])-float(calibrate.srgb_to_linear(180))) > .04
    assert not meta['raw'] and meta['profile'] == 'Embedded ICC → sRGB'


def test_bad_icc_is_rejected():
    from ventanal.calibration_photo import decode
    data=BytesIO(); Image.new('RGB',(10,10)).save(data,'PNG',icc_profile=b'invalid')
    with pytest.raises(ValueError): decode(data.getvalue(),'photo.png')


def test_upload_linear_storage_and_result(tmp_path,monkeypatch):
    from ventanal import calibration_ui
    from ventanal.calibration_photo import decode
    files=CalibrationFiles(tmp_path,'fixture')
    files.upload(dng(4000),'photo.dng')
    stored=np.load(files.directory/'linear.npy',allow_pickle=False)
    np.testing.assert_array_equal(stored,decode(dng(4000),'photo.dng')[0])
    assert (files.directory/'linear.npy').stat().st_mode & 0o777 == 0o600
    monkeypatch.setattr(calibration_ui,'render_comparison',lambda *a:None)
    files.measure({'corners':'0,0 63,0 63,63 0,63','paper':'10,10,20,20'},None)
    result=calibration_ui.read_measurement(files)
    assert result['raw_photo'] and result['paper_used']
    assert calibration_ui.MAX_UPLOAD == 150*1024*1024
    files.discard(); assert not files.directory.exists()


def test_linear_paper_mean_not_encoded_mean(tmp_path):
    from ventanal.calibration_photo import paper_mean
    pixels=calibrate.srgb_to_linear(np.array([[[0,0,0],[255,255,255]]]))
    np.testing.assert_allclose(paper_mean(pixels,(0,0,2,1)),(.5,.5,.5))


def test_real_heic_gainmap_is_ignored():
    from ventanal.calibration_photo import decode
    data=(PROFILE.parent/'gainmap.heic').read_bytes()
    linear,meta=decode(data,'gainmap.heic')
    with Image.open(BytesIO(data)) as primary:
        assert primary.info.get('icc_profile')
        reference=ImageCms.profileToProfile(primary.convert('RGB'),
            ImageCms.ImageCmsProfile(BytesIO(primary.info['icc_profile'])),
            ImageCms.createProfile('sRGB'),outputMode='RGB',renderingIntent=1)
    np.testing.assert_allclose(linear,calibrate.srgb_to_linear(reference),atol=1e-6)
    assert 'SDR base' in meta['note'] and 'gain maps ignored' in meta['note']


def test_raw_decode_parameters(monkeypatch):
    from ventanal.calibration_photo import decode
    import rawpy
    actual=rawpy.imread
    calls=[]
    class Raw:
        def __init__(self,data): self.raw=actual(data); self.sizes=self.raw.sizes
        def __enter__(self): return self
        def __exit__(self,*args): self.raw.close()
        def postprocess(self,**kwargs):
            calls.append(kwargs)
            return self.raw.postprocess(**kwargs)
    monkeypatch.setattr(rawpy,'imread',Raw)
    decode(dng(4000),'photo.dng')
    options=calls[0]
    assert options['gamma']==(1,1) and options['no_auto_bright']
    assert not options['use_camera_wb'] and not options['use_auto_wb']
    assert options['user_wb']==[1,1,1,1] and options['output_bps']==16
    assert options['output_color']==rawpy.ColorSpace.sRGB
    assert options['highlight_mode']==rawpy.HighlightMode.Ignore
    assert options['adjust_maximum_thr']==0


def test_real_ultrahdr_jpeg_uses_sdr_base_only():
    from ventanal.calibration_photo import decode
    data=(PROFILE.parent/'ultrahdr.jpg').read_bytes()
    assert b'hdr-gain-map' in data and data.count(b'\xff\xd8')==2
    linear,meta=decode(data,'android.jpg')
    with Image.open(BytesIO(data)) as source:
        profile=source.info.get('icc_profile')
        base=source.convert('RGB')
        if profile:
            base=ImageCms.profileToProfile(base,ImageCms.ImageCmsProfile(BytesIO(profile)),
                ImageCms.createProfile('sRGB'),outputMode='RGB',renderingIntent=1)
        np.testing.assert_allclose(linear,calibrate.srgb_to_linear(base),atol=1e-6)
        offset=data.index(b'\xff\xd8',2)
        with Image.open(BytesIO(data[offset:])) as auxiliary:
            gain=np.asarray(auxiliary.convert('RGB'))
        assert not np.array_equal(np.asarray(base),gain)
    assert 'SDR base' in meta['note'] and 'gain maps ignored' in meta['note']



def test_raw_orientation_is_normalized_once():
    from ventanal.calibration_photo import decode
    upright,_=decode(dng(4000,size=(64,96)), 'photo.dng')
    rotated,_=decode(dng(4000,size=(64,96),orientation=6), 'photo.dng')
    assert upright.shape==(64,96,3)
    assert rotated.shape==(96,64,3)


def test_p3_pure_red_is_identified_as_outside_srgb():
    from ventanal.calibration_photo import decode
    data=BytesIO()
    Image.new('RGB',(16,16),(255,0,0)).save(data,'PNG',icc_profile=PROFILE.read_bytes())
    pixels,meta=decode(data.getvalue(),'red.png')
    # Pure P3 red lies outside sRGB. An RGB8 sRGB transform clips it to red;
    # identify that loss rather than presenting unchanged numbers as accurate.
    assert meta['out_of_srgb']
    assert 'outside sRGB' in meta['note']
    np.testing.assert_array_equal(pixels[0,0],(1,0,0))


def png16(samples, profile=None):
    """Build a real RGB16 PNG without an encoder that downconverts to RGB8."""
    import struct
    import zlib

    def chunk(kind, payload):
        return (struct.pack('>I', len(payload)) + kind + payload
                + struct.pack('>I', zlib.crc32(kind + payload)))

    height, width, _ = samples.shape
    header = struct.pack('>IIBBBBB', width, height, 16, 2, 0, 0, 0)
    rows = b''.join(b'\0' + row.astype('>u2').tobytes() for row in samples)
    icc = chunk(b'iCCP', b'profile\0\0' + zlib.compress(profile)) if profile else b''
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + icc
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


@pytest.mark.parametrize('fmt', ['png', 'tiff', 'gray-tiff', 'gray-png'])
def test_16bit_processed_preserves_precision(fmt):
    from ventanal.calibration_photo import decode
    samples = np.tile(np.array([10000, 10001, 32768], dtype=np.uint16), (4, 5, 1))
    if fmt.startswith('gray'):
        samples = samples[:, :, 0]
    if fmt == 'png':
        data = png16(samples)
    else:
        output = BytesIO()
        if fmt.endswith('png'):
            Image.fromarray(samples).save(output, 'PNG')
        else:
            tifffile.imwrite(output, samples, metadata=None)
        data = output.getvalue()
    pixels, _ = decode(data, 'photo.' + fmt.split('-')[-1])
    encoded = samples.astype(np.float64) / 65535
    expected = np.where(encoded <= .04045, encoded / 12.92,
                        ((encoded + .055) / 1.055) ** 2.4)
    if samples.ndim == 2:
        expected = np.repeat(expected[:, :, None], 3, axis=2)
    np.testing.assert_allclose(pixels, expected, atol=1e-7, rtol=0)
    assert pixels.dtype == np.float32
    if samples.ndim == 3:
        assert pixels[0, 0, 0] != pixels[0, 0, 1]


@pytest.mark.parametrize('fmt', ['png', 'tiff'])
def test_unsupported_16bit_profile_is_friendly(fmt):
    from ventanal.calibration_photo import decode
    samples = np.full((4, 5, 3), 32768, dtype=np.uint16)
    if fmt == 'png':
        data = png16(samples, PROFILE.read_bytes())
    else:
        output = BytesIO()
        tifffile.imwrite(output, samples, metadata=None,
                         extratags=[(34675, 'B', len(PROFILE.read_bytes()), PROFILE.read_bytes(), False)])
        data = output.getvalue()
    with pytest.raises(ValueError, match='16-bit.*ICC.*RAW'):
        decode(data, 'photo.' + fmt)


def test_old_session_requests_reupload(tmp_path):
    files = CalibrationFiles(tmp_path, 'old-session')
    files.directory.mkdir(parents=True)
    Image.new('RGB', (10, 10)).save(files.directory / 'photo.png')
    Image.new('RGB', (10, 10)).save(files.directory / 'preview.png')
    with pytest.raises(ValueError, match='Please upload the photo again'):
        files.measure({'corners': '0,0 9,0 9,9 0,9'}, None)


def test_public_srgb_linearisation():
    np.testing.assert_allclose(calibrate.srgb_to_linear([0, 32768, 65535], maximum=65535),
                               [0, .2140482023, 1], atol=1e-9)


def test_16bit_tiff_orientation_once():
    from ventanal.calibration_photo import decode
    samples = np.arange(60, dtype=np.uint16).reshape(4, 5, 3) * 1000
    output = BytesIO()
    tifffile.imwrite(output, samples, metadata=None,
                     extratags=[(274, 'H', 1, 6, False)])
    pixels, _ = decode(output.getvalue(), 'photo.tiff')
    expected = calibrate.srgb_to_linear(np.rot90(samples, -1), maximum=65535)
    np.testing.assert_allclose(pixels, expected, atol=1e-7)


@pytest.mark.parametrize('dtype', [np.float32, np.int16, np.uint32])
def test_unsupported_tiff_samples_are_friendly(dtype):
    from ventanal.calibration_photo import decode
    output = BytesIO()
    tifffile.imwrite(output, np.ones((4, 5), dtype=dtype), metadata=None)
    with pytest.raises(ValueError, match='Upload .*unsupported'):
        decode(output.getvalue(), 'photo.tiff')
