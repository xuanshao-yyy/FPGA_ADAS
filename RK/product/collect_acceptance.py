"""Measure current product mode without restarting playback or changing rates."""
from pathlib import Path
import argparse,csv,json,os,statistics,time,urllib.request
def cpu(pid):
    try:
        row=Path('/proc/'+str(pid)+'/stat').read_text().rsplit(')',1)[1].split()
        return (int(row[11])+int(row[12]))/os.sysconf('SC_CLK_TCK')
    except (OSError,IndexError,ValueError):return None
def machine():
    p=Path('/proc/stat').read_text().splitlines()[0].split()[1:];v=list(map(int,p));return sum(v[:8]),v[3]+v[4]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--seconds',type=int,default=60);parser.add_argument('--url',default='http://127.0.0.1:8080');parser.add_argument('--state',type=Path,default=Path(__file__).resolve().parents[1]/'results/product_runtime');a=parser.parse_args()
    if not 5<=a.seconds<=3600:parser.error('seconds must be 5..3600')
    token=(a.state/'access_token.txt').read_text().strip();out=a.state/('acceptance_'+time.strftime('%Y%m%d_%H%M%S'));out.mkdir();rows=[];last={};old_machine=machine();previous=time.monotonic();began=previous
    with (out/'samples.csv').open('w',newline='') as file:
        fields=['elapsed_s','mode','front_hz','left_hz','right_hz','rear_hz','service_cpu_one_core_pct','drive_cpu_one_core_pct','parking_cpu_one_core_pct','machine_cpu_all_cores_pct','drive_running','parking_running','fpga_mode','error'];writer=csv.DictWriter(file,fields);writer.writeheader()
        for n in range(a.seconds):
            time.sleep(max(0,previous+1-time.monotonic()));now=time.monotonic();request=urllib.request.Request(a.url+'/api/status',headers={'X-Product-Token':token})
            with urllib.request.urlopen(request,timeout=3) as response:s=json.load(response)
            now=time.monotonic()
            channels=s.get('perception',{}).get('channels',[]);by_channel={v.get('channel'):round(v.get('hz',0),2) for v in channels};hz=[by_channel.get(ch,0) for ch in (0,2,1,3)]
            row=dict(zip(['front_hz','left_hz','right_hz','rear_hz'],hz));row.update(elapsed_s=round(now-began,3),mode=s['mode'],drive_running=s['drive_running'],parking_running=s['parking_running'],fpga_mode=(s.get('fpga') or {}).get('mode','UNCONFIRMED'),error=s.get('error',''))
            for name,key in [('service','service_pid'),('drive','drive_pid'),('parking','parking_pid')]:
                pid=s.get(key);value=cpu(pid) if pid else None;old=last.get(name)
                row[name+'_cpu_one_core_pct']=round(100*(value-old[1])/(now-previous),2) if value is not None and old and old[0]==pid else ''
                last[name]=(pid,value) if value is not None else None
            current=machine();total=current[0]-old_machine[0];row['machine_cpu_all_cores_pct']=round(100*(1-(current[1]-old_machine[1])/total),2) if total>0 else 0;old_machine=current;previous=now;writer.writerow(row);file.flush();rows.append(row)
    drive=[r['front_hz'] for r in rows[5:] if r['mode']=='DRIVE'];summary=dict(samples=len(rows),drive_samples_after_warmup=len(drive),front_mean_hz=statistics.mean(drive) if drive else None,front_min_hz=min(drive) if drive else None,front_fraction_ge10=sum(v>=10 for v in drive)/len(drive) if drive else None,notes=['100% process CPU = one core; machine CPU uses all cores','This is measurement, not automatic scene-accuracy acceptance','Parking samples do not count as front-YOLO throughput'])
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(out);print(json.dumps(summary,indent=2))
if __name__=='__main__':main()
