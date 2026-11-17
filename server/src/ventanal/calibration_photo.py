# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Decode original calibration files into linear sRGB, separately from previews.

LibRaw uses the camera matrix, neutral WB and 16-bit linear output. A future
ColorChecker 3×3 correction matrix belongs immediately after this RAW decode;
the reference-card workflow is deliberately deferred.
"""
from io import BytesIO
from pathlib import Path
import warnings

import numpy as np
from PIL import Image, ImageCms, ImageOps
import pillow_heif
import rawpy
import png
import tifffile

from ventanal.calibrate import srgb_to_linear

pillow_heif.register_heif_opener(thumbnails=False, decode_threads=2)
RAW_EXTENSIONS = {'.dng','.cr2','.cr3','.nef','.arw','.raf','.orf','.rw2'}
PROCESSED_EXTENSIONS = {'.jpg','.jpeg','.png','.tif','.tiff','.heic','.heif'}
RAW_TIER = 'RAW — highest accuracy'
PROCESSED_TIER = 'Processed photo — less accurate (phone processing such as tone mapping/HDR cannot be undone); use RAW if you can'
SDR_NOTE = 'SDR base image measured only; HDR gain maps ignored.'
_SRGB_LINEAR = srgb_to_linear(np.arange(256)).astype(np.float32)


def encode(linear):
    """Clip only at the final sRGB palette/display boundary."""
    linear = np.asarray(linear)
    if linear.ndim == 3 and linear.shape[0] > 256:
        # Large RAWs must not allocate several full-size float temporaries just
        # to make the 8-bit picker image. Measurement keeps the original floats.
        result = np.empty(linear.shape,dtype=np.uint8)
        for start in range(0,len(linear),256):
            result[start:start+256] = encode(linear[start:start+256])
        return result
    values = np.clip(linear,0,1)
    encoded = np.where(values<=.0031308,12.92*values,1.055*values**(1/2.4)-.055)
    return np.rint(encoded*255).astype(np.uint8)


def balance(samples, reference):
    reference=np.asarray(reference,dtype=float)
    if reference.shape!=(3,) or not np.isfinite(reference).all() or np.any(reference<=0):
        raise ValueError('paper channels must be positive and finite')
    return samples * srgb_to_linear(243)/reference


def paper_mean(pixels, rectangle):
    if len(rectangle)!=4 or not np.isfinite(rectangle).all():
        raise ValueError('paper must be x,y,width,height')
    x,y,w,h=map(int,rectangle)
    if x<0 or y<0 or w<=0 or h<=0 or x+w>pixels.shape[1] or y+h>pixels.shape[0]:
        raise ValueError('paper rectangle must lie inside the photo and have positive size')
    return pixels[y:y+h,x:x+w].mean(axis=(0,1),dtype=np.float64)


def outside_srgb(original, converted, profile):
    """Conservative sampled Lab round-trip check for visible gamut clipping.

    RGB8 sRGB cannot represent P3 pure red. Compare colourimetric Lab values
    rather than mistake identical clipped RGB numbers for a successful match.
    This is a warning, not a correction or an exhaustive per-pixel gamut mask.
    """
    original=original.copy(); original.thumbnail((64,64))
    converted=converted.copy(); converted.thumbnail((64,64))
    lab=ImageCms.createProfile('LAB')
    before=np.asarray(ImageCms.profileToProfile(original,profile,lab,
        outputMode='LAB',renderingIntent=1),dtype=float)
    after=np.asarray(ImageCms.profileToProfile(converted,ImageCms.createProfile('sRGB'),lab,
        outputMode='LAB',renderingIntent=1),dtype=float)
    return bool(np.any(np.linalg.norm(before-after,axis=2)>4))


class UnsupportedPhoto(ValueError):
    """A safe, actionable decoder limitation for the upload page."""


def high_depth_pixels(source, data):
    """Preserve PNG/TIFF precision before Pillow's RGB8 conversions.

    LittleCMS through Pillow cannot transform RGB16 ICC data losslessly, so
    reject that combination rather than silently measuring an 8-bit proxy.
    """
    if source.format == 'PNG':
        if data[24] != 16:
            return None
        if source.info.get('icc_profile'):
            raise UnsupportedPhoto('Upload a different photo: 16-bit PNG/TIFF with ICC profiles is unsupported; use a RAW original.')
        width, height, rows, info = png.Reader(bytes=data).asDirect()
        samples = np.vstack([np.asarray(row, dtype=np.uint16) for row in rows])
        samples = samples.reshape(height, width, info['planes'])
        if info['greyscale']:
            samples = np.repeat(samples[:, :, :1], 3, axis=2)
        else:
            samples = samples[:, :, :3]
    elif source.format == 'TIFF':
        bits = source.tag_v2.get(258, (8,))
        if isinstance(bits, int):
            bits = (bits,)
        if max(bits) <= 8 and source.mode != 'F':
            return None
        if source.info.get('icc_profile'):
            raise UnsupportedPhoto('Upload a different photo: 16-bit PNG/TIFF with ICC profiles is unsupported; use a RAW original.')
        with tifffile.TiffFile(BytesIO(data)) as image:
            page = image.pages[0]
            if (set(bits) != {16} or page.sampleformat != 1
                    or page.photometric not in (1, 2) or page.planarconfig != 1):
                raise UnsupportedPhoto('Upload a RAW original or unsigned 16-bit grayscale/RGB PNG or TIFF; this TIFF sample layout is unsupported.')
            samples = page.asarray()
        if samples.dtype.kind != 'u' or samples.dtype.itemsize != 2:
            raise ValueError('Unsupported TIFF sample type')
        if samples.ndim == 2:
            samples = np.repeat(samples[:, :, None], 3, axis=2)
        elif samples.ndim == 3 and samples.shape[2] in (3, 4):
            samples = samples[:, :, :3]
        else:
            raise ValueError('Unsupported TIFF channels')
    else:
        return None
    # Normalize EXIF orientation once, retaining each channel's 16-bit mode.
    orientation = source.getexif().get(274, 1)
    channels = []
    for channel in np.moveaxis(samples, 2, 0):
        plane = Image.fromarray(channel)
        plane.getexif()[274] = orientation
        channels.append(np.asarray(ImageOps.exif_transpose(plane)))
    return srgb_to_linear(np.stack(channels, axis=2), maximum=65535).astype(np.float32)


def decode(data, filename):
    """Return float32 linear RGB and safe provenance; never use RAW thumbnails.

    Processed decoders use the primary image only, never auxiliary/gain-map
    frames. ICC transforms use relative colorimetric intent (out-of-gamut values
    are clipped by LittleCMS); missing profiles alone imply sRGB. Malformed
    profiles fail rather than silently interpreting their samples as sRGB.
    """
    suffix=Path(filename).suffix.lower()
    try:
        if suffix in RAW_EXTENSIONS:
            with rawpy.imread(BytesIO(data)) as raw:
                if raw.sizes.width*raw.sizes.height > Image.MAX_IMAGE_PIXELS:
                    raise ValueError('RAW image is too large')
                rgb=raw.postprocess(gamma=(1,1), no_auto_bright=True,
                    use_camera_wb=False,use_auto_wb=False,user_wb=[1,1,1,1],
                    output_color=rawpy.ColorSpace.sRGB,output_bps=16,
                    highlight_mode=rawpy.HighlightMode.Ignore,adjust_maximum_thr=0)
            pixels=rgb.astype(np.float32)/65535
            meta={'raw':True,'profile':'Camera matrix → linear sRGB',
                  'note':'16-bit linear RAW; no camera/auto white balance or auto brightness. Saturated sensor values cannot be recovered.'}
        elif suffix in PROCESSED_EXTENSIONS:
            with warnings.catch_warnings():
                warnings.simplefilter('error',Image.DecompressionBombWarning)
                with Image.open(BytesIO(data)) as source:
                    if source.format not in ('JPEG','MPO','PNG','TIFF','HEIF') or 50706 in source.getexif():
                        raise ValueError('Unsupported processed format')
                    # A gain-map HEIC has an SDR primary plus auxiliary image;
                    # pillow-heif does not apply auxiliary gain maps. JPEG/MPO
                    # is likewise measured at frame zero, without seeking.
                    profile=source.info.get('icc_profile')
                    pixels = high_depth_pixels(source, data)
                    clipped = False
                    if pixels is None:
                        photo = ImageOps.exif_transpose(source)
                        if profile:
                            embedded = ImageCms.ImageCmsProfile(BytesIO(profile))
                            original = photo
                            photo = ImageCms.profileToProfile(original, embedded,
                                ImageCms.createProfile('sRGB'), outputMode='RGB', renderingIntent=1)
                            clipped = outside_srgb(original, photo, embedded)
                        else:
                            photo = photo.convert('RGB')
                        pixels = _SRGB_LINEAR[np.asarray(photo)]
            meta={'raw':False,'profile':'Embedded ICC → sRGB' if profile else 'No ICC profile; assumed sRGB',
                  'note':SDR_NOTE + (' Some sampled colours are outside sRGB and were clipped.' if clipped else ''),
                  'out_of_srgb':clipped}
        else:
            raise ValueError('Unsupported extension')
        if min(pixels.shape[:2])<2 or not np.isfinite(pixels).all():
            raise ValueError('Invalid image')
        return pixels,meta
    except UnsupportedPhoto:
        raise
    except (png.Error, NotImplementedError, IndexError, rawpy.LibRawError,OSError,ValueError,TypeError,ImageCms.PyCMSError,
            Image.DecompressionBombError,Image.DecompressionBombWarning):
        raise ValueError('Upload an original RAW, HEIC/HEIF, JPEG, PNG or TIFF with a valid colour profile.') from None
