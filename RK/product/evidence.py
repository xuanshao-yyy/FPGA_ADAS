"""Bounded event evidence and export; reuse encoded GOPs, never re-encode."""
from pathlib import Path
import json,queue,re,secrets,shutil,tempfile,threading,time,zipfile
from calibration import bmp565
SAFE=re.compile(r'^[a-zA-Z0-9_-]{1,70}$')
class Evidence:
    def __init__(self,state,fragments):
        self.root=Path(state)/'evidence';self.root.mkdir(parents=True,exist_ok=True);self.fragments=fragments
        self.jobs=queue.Queue(maxsize=1);self.last=-100.;self.error='';self.stop=threading.Event();self.worker=threading.Thread(target=self._run,daemon=True);self.worker.start()
    def capture_clip(self):
        now=time.monotonic()
        if now-self.last<10:return ''
        parts=self.fragments.evidence()
        if not parts:return ''
        name='park_'+secrets.token_hex(10)
        try:self.jobs.put_nowait((name,parts));self.last=now;return name
        except queue.Full:return ''
    def _run(self):
        while not self.stop.is_set() or not self.jobs.empty():
            try:name,parts=self.jobs.get(timeout=.2)
            except queue.Empty:continue
            temp=self.root/(name+'.mp4.tmp')
            try:
                if shutil.disk_usage(self.root).free<512*1024**2:raise OSError('证据保存暂停：剩余空间不足512 MiB')
                with temp.open('wb') as f:
                    for part in parts:f.write(part)
                temp.replace(self.root/(name+'.mp4'))
                files=sorted(self.root.glob('park_*.mp4'),key=lambda p:p.stat().st_mtime);total=sum(p.stat().st_size for p in files)
                while files and (len(files)>16 or total>32*1024**2):p=files.pop(0);total-=p.stat().st_size;p.unlink()
                self.error=''
            except OSError as e:self.error=str(e)
            finally:
                if temp.exists():temp.unlink()
                self.jobs.task_done()
    def file(self,name):
        if not SAFE.fullmatch(name):raise ValueError('无效证据ID')
        for ext in ('.rgb565','.mp4'):
            p=self.root/(name+ext)
            if p.is_file() and p.resolve().parent==self.root.resolve():return p
        return None
    def content(self,name):
        p=self.file(name)
        if not p:raise ValueError('证据尚未保存或已按容量轮换')
        return (bmp565(p.read_bytes()),'image/bmp') if p.suffix=='.rgb565' else (p.read_bytes(),'video/mp4')
    def decorate(self,rows):
        return [dict(row,evidence_available=bool(row.get('image_id') and self.file(row['image_id']))) for row in rows]
    def export(self,rows,status):
        # Fixed small, uncompressed archive avoids RK JPEG/ZIP CPU spikes.
        file=tempfile.SpooledTemporaryFile(max_size=1024**2,dir=self.root.parent)
        with zipfile.ZipFile(file,'w',zipfile.ZIP_STORED) as z:
            z.writestr('events.json',json.dumps(rows,ensure_ascii=False,indent=2));z.writestr('status.json',json.dumps(status,ensure_ascii=False,indent=2))
            z.writestr('README.txt','Simulation/advisory evidence. Images are nearby received frames, not guaranteed inference-frame matches. Parking clips contain the latest complete encoded GOP and may be short. No keys, original media or full service logs included.\n')
            size=0;count=0
            for row in rows:
                p=self.file(row['image_id']) if row.get('image_id') else None
                if not p or size+p.stat().st_size>20*1024**2 or count>=8:continue
                if p.suffix=='.rgb565':z.writestr('evidence/'+p.stem+'.bmp',bmp565(p.read_bytes()))
                else:z.write(p,'evidence/'+p.name)
                size+=p.stat().st_size;count+=1
        file.seek(0,2);size=file.tell();file.seek(0);return file,size
    def close(self):self.stop.set();self.worker.join(timeout=5)
