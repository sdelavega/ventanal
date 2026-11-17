# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Stephen de la Vega
"""Government public products, checked 2026-11-16. No call sign in requests.

Daily solar text supplies daily F10.7 and SESC sunspots; the f107 JSON
was stale at endpoint verification. Kp includes running planetary A.
SDO 171 Å highlights coronal structure with strong yellow/black contrast.
"""
from dataclasses import dataclass
from datetime import datetime, timezone, timedelta
from io import BytesIO
import json
import os
import re
from PIL import Image
from ventanal.weather.data import USER_AGENT

BASE = 'https://services.swpc.noaa.gov/'
PRODUCTS = {
    'daily': ('text/daily-solar-indices.txt',3600),
    'kp': ('products/noaa-planetary-k-index.json',900),
    'xray': ('json/goes/primary/xrays-6-hour.json',600),
    'forecast': ('text/3-day-forecast.txt',1800),
    'alerts': ('products/alerts.json',300),
    'scales': ('products/noaa-scales.json',300),
}
SDO = 'https://sdo.gsfc.nasa.gov/assets/img/latest/latest_1024_0171.jpg'

@dataclass
class Solar:
    sfi: float
    ssn: int
    xray: str
    k: float
    a: int
    kp: list
    forecast: str
    alerts: list
    scales: dict
    sun: Image.Image
    sun_day: str
    daily_day: str = ''


def xray_class(flux):
    if flux < 0: return 'UNKNOWN'
    for label,base in [('X',1e-4),('M',1e-5),('C',1e-6),('B',1e-7),('A',1e-8)]:
        if flux>=base or label=='A': return f'{label}{flux/base:.1f}'


def cached(client, cache, name, url, ttl, now):
    cache.mkdir(parents=True, exist_ok=True)
    target=cache/name
    if target.exists() and 0 <= now.timestamp()-target.stat().st_mtime < ttl: return target.read_bytes()
    r=client.get(url,timeout=45,follow_redirects=True,headers={"User-Agent": USER_AGENT}); r.raise_for_status()
    if len(r.content)>16*1024*1024: raise ValueError('Solar product too large')
    tmp=target.with_suffix('.tmp'); tmp.write_bytes(r.content); os.replace(tmp,target)
    os.utime(target,(now.timestamp(),now.timestamp()))
    return r.content


def active_alerts(rows, now):
    """Latest NOAA product only: explicit warning expiry, dated storm watches,
    and recent alerts (6h; continued alerts 36h). Old history is never replayed.
    Watches list UTC forecast days rather than a Valid To timestamp.
    """
    result=[]
    latest={}
    for row in sorted(rows,key=lambda r:r['issue_datetime']): latest[row['product_id']]=row
    for row in latest.values():
        message=row['message']
        if 'CANCEL' in message.upper(): continue
        title=re.search(r'(?m)^(?:(?:EXTENDED|CONTINUED) )?(?:WATCH|WARNING|ALERT):[^\r\n]+',message)
        if not title: continue
        end=re.search(r'(?:Valid To|Now Valid Until):\s*(\d{4} \w{3} \d{1,2} \d{4}) UTC',message)
        if end:
            expires=datetime.strptime(end[1],'%Y %b %d %H%M').replace(tzinfo=timezone.utc)
            if expires>now: result.append(title[0].strip()+' · UNTIL '+expires.strftime('%b %d %H%M UTC'))
        elif title[0].startswith('WATCH:'):
            days=[]
            for month,day,level in re.findall(r'([A-Z][a-z]{2}) (\d{1,2}):\s+(G[1-5])',message):
                # Choose the nearest year to the issue date (handles December/January).
                issue=datetime.fromisoformat(row['issue_datetime']).replace(tzinfo=timezone.utc)
                dates=[datetime.strptime(f'{year} {month} {day}','%Y %b %d').replace(tzinfo=timezone.utc) for year in (issue.year-1,issue.year,issue.year+1)]
                date=min(dates,key=lambda date:abs((date-issue).total_seconds()))
                if date.date()>=now.date(): days.append(f'{month} {day} {level}')
            if days: result.append(title[0].strip()+' · '+', '.join(days))
        elif 'ALERT:' in title[0]:
            issue=datetime.fromisoformat(row['issue_datetime']).replace(tzinfo=timezone.utc)
            age=(now-issue).total_seconds()
            limit=36*3600 if title[0].startswith('CONTINUED') else 6*3600
            if 0<=age<limit: result.append(title[0].strip()+' · '+issue.strftime('%b %d %H%M UTC'))
    # HF-relevant storm/radiation products precede routine electron notices.
    return sorted(set(result),key=lambda value:("electron" in value.lower(),value))


def fetch_solar(client, cache, now):
    cache=cache/'solar'
    values={}
    for name,(path,ttl) in PRODUCTS.items():
        raw=cached(client,cache,name,BASE+path,ttl,now)
        values[name]=raw.decode() if name in ('daily','forecast') else json.loads(raw)
    rows=[line.split() for line in values['daily'].splitlines() if re.match(r'^\d{4}\s+\d{2}\s+\d{2}',line)]
    row=max((r for r in rows if float(r[3])>0 and int(r[4])>=0),key=lambda r:r[:3])
    kp=values['kp']
    if isinstance(kp[0],list): kp=[dict(zip(kp[0],r)) for r in kp[1:]]
    kp=sorted(kp,key=lambda r:r['time_tag'])[-8:]
    xray=max((r for r in values['xray'] if r['energy']=='0.1-0.8nm' and r['flux']>=0),key=lambda r:r['time_tag'])
    day=now.astimezone(timezone.utc).date().isoformat()
    # Daily filename ensures at most one SDO update per UTC day.
    image=cached(client,cache,'sun-'+day+'.jpg',SDO,172800,now)
    for old in cache.glob('sun-*.jpg'):
        if old.name != 'sun-'+day+'.jpg': old.unlink()
    sun=Image.open(BytesIO(image)).convert('RGB'); sun.load()
    return Solar(float(row[3]),int(row[4]),xray_class(xray['flux']),float(kp[-1]['Kp']),int(kp[-1]['a_running']),
                 [(r['time_tag'],float(r['Kp'])) for r in kp],values['forecast'],
                 active_alerts(values['alerts'],now),values['scales'],sun,day,'-'.join(row[:3]))


def forecast_days(s):
    """Extract official per-day Kp maxima alongside NOAA scale probabilities."""
    maxima=[0.,0.,0.]
    for line in s.forecast.splitlines():
        if re.match(r'^\d{2}-\d{2}UT',line):
            numbers=re.findall(r'\d+\.\d+',line)
            for i,n in enumerate(numbers[:3]): maxima[i]=max(maxima[i],float(n))
    result=[]
    for i in range(3):
        scale=s.scales.get(str(i+1),{})
        result.append((scale.get('DateStamp','UNKNOWN'), maxima[i],
                       scale.get('R',{}).get('MinorProb'),scale.get('S',{}).get('Prob'),
                       scale.get('G',{}).get('Scale')))
    return result
