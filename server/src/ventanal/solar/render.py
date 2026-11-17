# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Solar content within the shared weather-approved frame. All adjustable groups are in LAYOUT/BANDS."""
from datetime import datetime, timezone
import re
from ventanal.text import font
from PIL import Image, ImageDraw
from ventanal.quantize import Canvas, Method
from ventanal.manifest import Readout
from ventanal.palette import LCARS_MODERN, PANEL_INDEX
from ventanal.weather.render import _text
from ventanal.weather.lcars import draw_frame, pill, sidebar_ink
from ventanal.solar.conditions import conditions, grid_square
from ventanal.solar.data import forecast_days

SOLAR_READOUT=Readout(1236,30,334,136,'lcars')
# Sidebar segments have 6 px gaps; the last ends 6 px before the bottom elbow.
LAYOUT = {
    'sidebar': ((212,126,'sky','SOLAR FLUX'),(344,146,'red','GEOMAG'),(496,264,'gold','HF BANDS'),(766,114,'sky','KP 24 H'),(886,102,'violet','3-DAY')),
    'sun': (1170,224,340), 'bands': (330,522,66),
    'chart': (365,814,1200,90), 'forecast': (330,1008,415),
    'alert': (330,1082,1240,22),
}


def integer_k(k):
    """Nearest integer planetary K, with halves rounded upward."""
    return max(0,min(9,int(float(k)+0.5)))


def kp_notation(k):
    thirds=max(0,min(27,int(float(k)*3+0.5)))
    whole,remainder=divmod(thirds,3)
    return str(whole) if remainder==0 else f'{whole}+' if remainder==1 else f'{whole+1}-'


def k_color(k):
    return 'green' if k<=3 else 'yellow' if k<5 else 'red'


def forecast_label(k,g):
    level=int(g or 0)
    if level: return f'G{level} STORM'
    if k>=5: return f'G{min(5, max(1,integer_k(k)-4))} STORM'
    return 'ACTIVE' if k>4 else 'UNSETTLED' if k>3 else 'QUIET'


def crop_sun(sun):
    """Remove SDO's bottom 4.5% timestamp margin with a centred square crop."""
    margin=round(min(sun.size)*0.045)
    return sun.crop((margin,margin,sun.width-margin,sun.height-margin))


def alert_summary(alerts):
    """At most two HF-relevant products; whole type, scale and UTC dates."""
    if not alerts: return '', 'NO ACTIVE SWPC WATCHES / WARNINGS'
    summaries=[]
    chosen=sorted(alerts,key=lambda a:('electron' in a.lower(), -max([int(n) for n in re.findall(r'[GRS]([1-5])',a)] or [0])))[:2]
    label='WATCH' if all('WATCH:' in a.upper() for a in chosen) else 'ALERT'
    for alert in chosen:
        upper=alert.upper()
        kind='WATCH' if 'WATCH:' in upper else 'WARNING' if 'WARNING:' in upper else 'ALERT'
        product=('GEOMAGNETIC STORM' if 'GEOMAGNETIC' in upper or 'K-INDEX' in upper else
                 'RADIO BLACKOUT' if 'X-RAY' in upper or 'RADIO' in upper else
                 'SOLAR RADIATION' if 'PROTON' in upper or 'RADIATION' in upper else
                 'ELECTRON FLUX' if 'ELECTRON' in upper else 'SPACE WEATHER')
        scales=list(dict.fromkeys(re.findall(r'\b[GRS][1-5]\b',upper)))
        if not scales and 'K-INDEX' in upper:
            match=re.search(r'K-INDEX\s+([5-9])',upper)
            if match: scales=[f'G{int(match[1])-4}']
        dates=list(dict.fromkeys(re.findall(r'\b(?:JAN|FEB|MAR|APR|MAY|JUN|JUL|AUG|SEP|OCT|NOV|DEC) \d{1,2}(?: \d{4} UTC)?',upper)))
        prefix=[kind] if kind!=label else []
        summaries.append(' '.join([*prefix,product,*scales])+(' · '+('UNTIL ' if 'UNTIL' in upper else '')+', '.join(dates) if dates else ''))
    return label, ' / '.join(summaries)


def alert_fit(message,width):
    """Keep 18 px type; omit secondary products, then elide whole words."""
    face=font('antonio',18)
    if face.getlength(message)<=width: return message,18
    products=message.split(' / ')
    while len(products)>1:
        products.pop()
        candidate=' / '.join(products)+' …'
        if face.getlength(candidate)<=width: return candidate,18
    words=products[0].split()
    while words:
        words.pop()
        candidate=' '.join(words)+(' ' if words else '')+'…'
        if face.getlength(candidate)<=width: return candidate,18
    return ('…' if face.getlength('…')<=width else ''),18


def render_solar(s,call,coords,now,cfg):
    c=Canvas(); p=c.region(Method.PATTERN); solid=c.region(Method.SOLID)
    ink={name:cfg.panel_rgb[index] for name,index in PANEL_INDEX.items()}
    m=LCARS_MODERN
    def text(label,x,y,size=20,color='white',bold=False,width=None,anchor='ls'):
        _text(c,str(label).upper(),(x,y),size,ink[color],bold=bold,max_w=width,anchor=anchor)
    draw_frame(c, [(y,h,color) for y,h,color,_ in LAYOUT['sidebar']],accent=m['orange'])
    for y,h,color,label in LAYOUT['sidebar']:
        _text(c,label,(248,y+h-12),22,sidebar_ink(color,cfg.panel_rgb),bold=True,anchor='rs')
    text('SOLAR CONDITIONS',330,148,52,'yellow',True)
    text(f'{call} · {grid_square(*coords)} · {now.astimezone(timezone.utc):%d %b %Y %H:%M} UTC · NOAA SWPC · NASA SDO',332,194,24,width=1238)
    def metric(label,value,x,y,size,color='white'):
        text(label,x,y,20)
        # Align visible glyph tops: different digits have different font bounds.
        label_bottom=font('antonio',20).getbbox(label,anchor='ls')[3]
        value_top=font('antonio-bold',size).getbbox(str(value),anchor='ls')[1]
        text(value,x,y+label_bottom+14-value_top,size,color,True)
    metric('SOLAR FLUX (SFI)',f'{s.sfi:.0f}',330,244,120)
    for label,value,x,y,color in [('SUNSPOTS',s.ssn,660,244,'white'),('X-RAY',s.xray,880,244,'yellow'),('K-INDEX',integer_k(s.k),660,356,k_color(integer_k(s.k))),('A-INDEX',s.a,880,356,'white')]:
        metric(label,value,x,y,56,color)
    est=conditions(s.sfi,s.k,s.a,s.xray)
    text(f'FIELD {est.field} · EST. NOISE {est.noise} · MUF ≈ {est.muf} MHZ',330,470,22,'white',width=800)
    x,y,step=LAYOUT['bands']; text('DAY',620,y-4,20); text('NIGHT',890,y-4,20)
    for i,(band,day,night) in enumerate(est.bands):
        top=y+i*step; text(band,x,top+29,28,'yellow',True)
        for bx,label in [(560,day),(830,night)]:
            color={'Good':'green','Fair':'yellow','Poor':'red'}[label]
            pill(solid,(bx,top,240,38),ink[color],True,True); _text(c,label.upper(),(bx+120,top+29),22,ink['black' if label=='Fair' else 'white'],bold=True,anchor='ms')
    text('HF CONDITIONS, MUF AND NOISE ARE ESTIMATES',330,783,18,'yellow')
    sx,sy,size=LAYOUT['sun']; sun=crop_sun(s.sun).resize((size,size),Image.Resampling.LANCZOS)
    mask=Image.new('1',sun.size); ImageDraw.Draw(mask).ellipse((0,0,size-1,size-1),fill=1)
    c.paste(sun,(sx,sy),Method.DIFFUSE,mask)
    text('SUN TODAY · NASA SDO 171 Å',sx+size/2,598,18,width=size,anchor='ms')
    current=s.scales.get('0',{})
    text('NOAA SCALES '+' · '.join(f"{key}{current.get(key,{}).get('Scale') or '0'}" for key in ('R','S','G'))+' (NOW)',sx+size/2,630,18,width=size,anchor='ms')
    x,y,w,h=LAYOUT['chart']
    for tick in (0,3,6,9):
        ty=y+h-tick/9*h
        p.line((x,ty,x+w,ty),fill=m['gray'],width=1)
        text(tick,x-25,ty+7,18)
    gy=y+h-5/9*h
    step=w/8
    for i,(stamp,k) in enumerate(s.kp[-8:]):
        height=max(4,min(9,max(0,k))/9*h)
        solid.rounded_rectangle((x+i*step,y+h-height,x+i*step+110,y+h),radius=2,fill=ink[k_color(k)])
        text(stamp[11:13],x+i*step+16,y+h+25,18)
    # Draw the threshold over bars; keep its label outside the data region.
    solid.line((x,gy,x+w,gy),fill=ink['yellow'],width=2)
    text('G1 (KP 5)',1450,947,18,'yellow',width=115)
    text('PLANETARY KP · 3-HOUR INTERVALS · UTC',330,947,18)
    x,y,step=LAYOUT['forecast']
    for i,(date,k,r,prob,g) in enumerate(forecast_days(s)):
        bx=x+i*step
        try: date=datetime.fromisoformat(date).strftime('%a %d')
        except (ValueError,TypeError): date='UNKNOWN'
        text(date,bx,y,32,'yellow',True)
        text(f'KP MAX {kp_notation(k)} · {forecast_label(k,g)}',bx,y+30,24)
        text(f'R1–R2 {r if r is not None else "?"}% · S1 {prob if prob is not None else "?"}%',bx,y+52,20)
    x,y,w,h=LAYOUT['alert']; pill(solid,(x,y,w,h),ink['black'],True,True)
    label,message=alert_summary(s.alerts)
    if label:
        text(label,x,y+18,20,'yellow',True)
        message,size=alert_fit(message,w-136)
        text(message,x+112,y+18,size,'white')
    else: text(message,x,y+18,20,'white')
    return c
