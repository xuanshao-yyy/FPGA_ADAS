"""Read a run without changing original CSV. Emit raw-UNKNOWN episodes and summary."""
from pathlib import Path
from collections import Counter
import argparse
import csv
import json


def summarize_lane(root):
    with (root/'lane_rules.csv').open(encoding='utf-8') as stream: rows=list(csv.DictReader(stream))
    if not rows or 'raw_valid' not in rows[0]:raise ValueError('Need competition-rules smooth lane_rules.csv')
    models=[r for r in rows if r['event']=='MODEL_OBSERVATION']
    unknown=[r for r in models if r['raw_valid']=='0']
    episodes=[]; start=None
    for r in rows:
        if r['raw_valid']=='0' and start is None:start=r
        elif r['raw_valid']=='1' and start is not None:
            episodes.append((start,r,False));start=None
    if start is not None:episodes.append((start,rows[-1],True))
    dest=root/'lane_unknown_intervals.csv'
    with dest.open('w',newline='',encoding='utf-8') as stream:
        out=csv.writer(stream);out.writerow(['start_s','end_s','duration_s','first_frame','first_source','reason','pair_reason','censored_at_end'])
        for a,b,c in episodes:out.writerow([a['now_s'],b['now_s'],max(0,float(b['now_s'])-float(a['now_s'])),a['frame_id'],a['source'],a['raw_reason'],a['pair_reason'],int(c)])
    result={'model_observations':len(models),'raw_unknown_observations':len(unknown),
            'raw_unknown_reasons':dict(Counter(r['raw_reason'] for r in unknown)),
            'raw_pair_reasons':dict(Counter(r['pair_reason'] for r in unknown)),
            'model_events_displaying_history':sum(r['position_held']=='1' for r in models),
            'unknown_episodes':len(episodes),'intervals_file':dest.name,
            'notes':'Raw UNKNOWN is retained, not model accuracy. Episode duration ends at next logged recovery or last log; final episode may be censored. Timeline follows receiver monotonic seconds.'}
    (root/'lane_unknown_summary.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    return result


def summarize_signal(root):
    source=root/'signal_auto_association.csv'
    if not source.is_file():return {'scope':'automatic signal CSV absent; inspect signal_observations.csv for manual mode'}
    with source.open(encoding='utf-8') as stream: rows=list(csv.DictReader(stream))
    report={};episodes=[]
    for direction in ('LEFT','STRAIGHT','RIGHT'):
        group=[r for r in rows if r['direction']==direction];start=None
        for r in group:
            if r['stable_color']=='UNKNOWN' and start is None:start=r
            elif r['stable_color']!='UNKNOWN' and start is not None:
                episodes.append((direction,start,r,False));start=None
        if start is not None:episodes.append((direction,start,group[-1],True))
        report[direction]={'observations':len(group),'raw_color_unknown':sum(r['color']=='UNKNOWN' for r in group),
                           'stable_color_unknown':sum(r['stable_color']=='UNKNOWN' for r in group),
                           'reasons':dict(Counter(r['reason'] for r in group if r['stable_color']=='UNKNOWN'))}
    with (root/'signal_unknown_intervals.csv').open('w',newline='',encoding='utf-8') as stream:
        out=csv.writer(stream);out.writerow(['direction','start_s','end_s','duration_s','first_frame','reason','censored_at_end'])
        for direction,a,b,c in episodes:out.writerow([direction,a['now_s'],b['now_s'],max(0,float(b['now_s'])-float(a['now_s'])),a['frame_id'],a['reason'],int(c)])
    result={'directions':report,'unknown_episodes':len(episodes),'notes':'Per-direction stable UNKNOWN episodes; unbound/unseen directions are expected UNKNOWN, not detector misses. STOP hold is recorded separately in signal_commands.csv.'}
    (root/'signal_unknown_summary.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');return result


def summarize(root):
    report={'signal':summarize_signal(root)}
    if (root/'lane_rules.csv').is_file():report['lane']=summarize_lane(root)
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('run',type=Path);args=parser.parse_args();print(json.dumps(summarize(args.run),indent=2))
