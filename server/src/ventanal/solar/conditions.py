# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""HF estimates, not a propagation model or safety forecast.

MUF MHz = clamp(10 + 0.1*SFI, 10, 45), rounded. K>=4 or A>=20
reduces each rating one step; K>=6 or A>=50 reduces two. Daytime
M/X absorption reduces another one/two steps. Noise is a K-only proxy.
Band thresholds and baseline day/night grades live in one editable table.
"""
from dataclasses import dataclass
import math

# name, minimum SFI for Fair/Good daytime, baseline night rating (0..2)
BANDS = (('80–40 M',0,999,2), ('30–20 M',70,100,2), ('17–15 M',90,120,1), ('12–10 M',120,170,0))
RATINGS = ('Poor','Fair','Good')

@dataclass(frozen=True)
class Conditions:
    bands: tuple
    muf: int
    noise: str
    field: str


def conditions(sfi,k,a,xray):
    penalty = 2 if k>=6 or a>=50 else 1 if k>=4 or a>=20 else 0
    absorption = 2 if xray.startswith('X') else 1 if xray.startswith('M') else 0
    rows=[]
    for name,fair,good,night in BANDS:
        day = 2 if sfi>=good else 1 if sfi>=fair else 0
        rows.append((name,RATINGS[max(0,day-penalty-absorption)],RATINGS[max(0,night-penalty)]))
    return Conditions(tuple(rows), round(max(10,min(45,10+0.1*sfi))),
                      'S1–S2' if k<3 else 'S3–S4' if k<5 else 'S5–S7' if k<7 else 'S8–S9',
                      'QUIET' if k<3 else 'UNSETTLED' if k<5 else 'STORM')


def grid_square(lat,lon):
    if not math.isfinite(lat) or not math.isfinite(lon) or not -90<=lat<=90 or not -180<=lon<=180:
        raise ValueError('Invalid coordinates')
    x=min(lon+180,359.999999); y=min(lat+90,179.999999)
    return chr(65+int(x//20))+chr(65+int(y//10))+str(int(x%20//2))+str(int(y%10))
