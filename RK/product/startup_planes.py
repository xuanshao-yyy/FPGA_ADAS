"""One-shot RK video-plane setup after the owned driving process presents a frame."""
from pathlib import Path
import json,os,re,subprocess,threading,time

def rk_selected(args,environ):
    selected=environ.get('PREVIEW_BACKEND','xv')
    for index,value in enumerate(args):
        if value=='--preview-sink' and index+1<len(args):selected=args[index+1]
    return selected=='rk'

def discover(text):
    blocks=re.findall(r'^plane\[(\d+)\]:[^\n]*\n(.*?)(?=^plane\[|^crtc\[|\Z)',text,re.M|re.S)
    primary=[int(i) for i,b in blocks if '\tcrtc=video_port0\n' in b and 'allocated by = Xorg' in b and 'format=XR24' in b]
    video=[int(i) for i,b in blocks if '\tcrtc=video_port0\n' in b and 'format=NV12' in b]
    if len(primary)!=1 or len(video)!=1:raise RuntimeError('Expected one HDMI Xorg primary and one NV12 plane')
    return primary[0],video[0]

def properties(text,ids):
    text=text[text.index('Planes:'):];out={};current=None;name=None
    for line in text.splitlines():
        match=re.match(r'^(\d+)\t',line)
        if match:current=int(match.group(1));name=None
        if current not in ids:continue
        match=re.match(r'\s+\d+ (zpos|colorkey):$',line)
        if match:name=match.group(1)
        elif name and 'value:' in line:
            out.setdefault(current,{})[name]=int(line.split('value:',1)[1].strip());name=None
    if any(set(out.get(i,{}))!={'zpos','colorkey'} for i in ids):raise RuntimeError('Missing DRM plane properties')
    return out

def owned_process(pid,launcher_pid,binary,proc=Path('/proc')):
    try:
        if (proc/str(pid)/'exe').resolve()!=binary.resolve():return False
        seen=set()
        while pid>1 and pid not in seen:
            if pid==launcher_pid:return True
            seen.add(pid);line=(proc/str(pid)/'stat').read_text();pid=int(line.rsplit(')',1)[1].split()[1])
    except (OSError,ValueError,IndexError):pass
    return False

def apply_once(state,launcher,started,stop,binary,timeout=20):
    state=Path(state);deadline=time.monotonic()+timeout;last_error='Preview not ready';pid=None
    while not stop.is_set() and launcher.poll() is None and time.monotonic()<deadline:
        try:
            ready=json.loads((state/'display_ready.json').read_text());pid=ready['pid']
            if not isinstance(pid,int) or ready.get('monotonic_s',-1)<started or not owned_process(pid,launcher.pid,binary):
                raise RuntimeError('Preview identity is stale or does not belong to this launcher')
            primary,video=discover(Path('/sys/kernel/debug/dri/0/state').read_text())
            ids=(primary,video);before=properties(subprocess.check_output(['modetest','-M','rockchip','-p'],text=True),ids)
            good=lambda p:p[primary]['zpos']==1 and p[primary]['colorkey']==0x80010203 and p[video]['zpos']==0
            if not good(before):
                if stop.is_set() or launcher.poll() is not None or not owned_process(pid,launcher.pid,binary):return
                subprocess.run(['modetest','-M','rockchip','-w',f'{primary}:colorkey:2147549699','-w',f'{primary}:zpos:1','-w',f'{video}:zpos:0'],check=True,stdout=subprocess.DEVNULL)
            after=properties(subprocess.check_output(['modetest','-M','rockchip','-p'],text=True),ids)
            if not good(after):raise RuntimeError('DRM property readback did not match')
            record={'status':'READY','pid':pid,'planes':[primary,video],'before':before,'after':after,'one_shot':True}
            (state/'startup_plane_status.json').write_text(json.dumps(record,indent=2)+'\n');return
        except (OSError,ValueError,KeyError,RuntimeError,subprocess.SubprocessError) as exc:last_error=str(exc)
        stop.wait(.1)
    if not stop.is_set() and launcher.poll() is None:
        record={'status':'FAILED','pid':pid,'error':last_error,'one_shot':True}
        (state/'startup_plane_status.json').write_text(json.dumps(record,indent=2)+'\n')
        print('RK startup plane setup failed: '+last_error,flush=True)

def launch_once(state,launcher,started,stop,binary,args,environ=None):
    if not rk_selected(args,os.environ if environ is None else environ):return None
    worker=threading.Thread(target=apply_once,args=(state,launcher,started,stop,binary),name='rk-startup-planes',daemon=True)
    worker.start();return worker
