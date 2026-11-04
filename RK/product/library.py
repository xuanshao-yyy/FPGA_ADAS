"""Bounded local/USB media preparation. Run only after the supervisor stops AI.
Original inputs are read-only; conversion is single-threaded, full-duration and
letterboxed. It is deliberately outside the realtime inference path.
"""
from pathlib import Path
import fractions,hashlib,json,math,os,shutil,subprocess,threading,time

FRAME=1280*720*2
MAX_RAW=8*1024**3
RESERVE=2*1024**3
EXT={'.mp4','.mov','.mkv','.avi','.jpg','.jpeg','.png','.bmp','.rgb565'}
IMAGES={'.jpg','.jpeg','.png','.bmp'}
def read(path,default=None):
    try:return json.loads(Path(path).read_text(encoding='utf8'))
    except (OSError,ValueError):return default
def write(path,value):
    temp=path.with_suffix('.json.tmp');temp.write_text(json.dumps(value,ensure_ascii=False),encoding='utf8');temp.replace(path)
def signature(path):
    s=path.stat();return (str(path.resolve()),s.st_size,s.st_mtime_ns)
def media_info(data,still=False):
    stream=next((s for s in data.get('streams',[]) if s.get('codec_type')=='video'),None)
    if not stream:raise ValueError('素材没有可解码图像流')
    if still:return dict(fps=1.,duration=0.,estimate=FRAME,still=True)
    try:rate=float(fractions.Fraction(stream.get('avg_frame_rate') or stream.get('r_frame_rate') or '0'))
    except (ValueError,ZeroDivisionError):rate=0
    duration=float(data.get('format',{}).get('duration') or stream.get('duration') or 0)
    if not math.isfinite(rate) or not 1<=rate<=240 or not math.isfinite(duration) or duration<=0:raise ValueError('无法确定完整视频时长或帧率')
    fps=min(30.,rate);estimate=math.ceil(duration*fps+2)*FRAME
    if estimate>MAX_RAW:raise ValueError('完整视频转换将超过8 GiB，未截短；请改用较低帧率的外部准备素材')
    return dict(fps=fps,duration=duration,estimate=estimate,still=False)

class Library:
    def __init__(self,runtime,state,roots=None):
        self.runtime=Path(runtime).resolve();self.cache=Path(state)/'media_cache';self.cache.mkdir(parents=True,exist_ok=True)
        self.roots=[Path(p).resolve() for p in (roots if roots is not None else [self.runtime/'video','/userdata/test_auto_drive/video','/media','/run/media','/mnt'])]
        self.lock=threading.RLock();self.files={};self.cancelled=threading.Event();self.thread=None;self.process=None
        self.job={'state':'IDLE','message':'选择素材后在RK显示器测试','progress':0};self.selected=None;self.finished=None
    def scan(self):
        found={};visited=0;known={}
        for row in read(self.runtime/'all_player/scene_manifest.json',{'scenes':[]})['scenes']:
            path=self.runtime/'video/replay'/row['asset_root']/row['file'];known[str(path.resolve())]=row['fps']
        for root in self.roots:
            if not root.is_dir():continue
            for base,dirs,names in os.walk(str(root),followlinks=False):
                path=Path(base);depth=len(path.relative_to(root).parts);dirs[:]=[d for d in sorted(dirs) if not d.startswith('.')][:24] if depth<3 else []
                for name in sorted(names):
                    visited+=1
                    if visited>2000 or len(found)>=400:break
                    p=path/name
                    try:
                        real=p.resolve(strict=True)
                        if p.suffix.lower() not in EXT or not real.is_file() or root not in real.parents:continue
                        identity=signature(real);key=hashlib.sha256(repr(identity).encode()).hexdigest()[:20]
                        found[key]={'id':key,'name':name,'path':str(real),'bytes':identity[1],'signature':identity,'fps':known.get(str(real)),'ready':bool(read(self.cache/(key+'.json')))}
                    except OSError:continue
                if visited>2000 or len(found)>=400:break
        with self.lock:self.files=found
        return [{k:v for k,v in row.items() if k not in ('signature',)} for row in found.values()]
    def state(self):
        with self.lock:return dict(self.job,selected=self.selected,cache_free_bytes=shutil.disk_usage(self.cache).free,raw_limit_bytes=MAX_RAW)
    def begin(self,key,raw_fps=24):
        with self.lock:
            if self.thread and self.thread.is_alive():raise ValueError('已有素材准备任务')
            row=self.files.get(key)
            if not row:raise ValueError('请先刷新素材列表并选择有效条目')
            self.cancelled.clear();self.finished=None;self.job={'state':'PREPARING','message':'正在检查素材，识别已暂停','progress':0,'name':row['name']}
            row=dict(row);row["raw_fps"]=row.get("fps") or raw_fps
            self.thread=threading.Thread(target=self._prepare,args=(row,),daemon=True);self.thread.start()
    def _set(self,**values):
        with self.lock:self.job.update(values)
    def _prepare(self,row):
        key=row['id'];part=self.cache/(key+'.rgb565.part');log=self.cache/(key+'.log')
        try:
            source=Path(row['path'])
            if tuple(row['signature'])!=signature(source):raise ValueError('素材已改变，请刷新列表')
            cached=read(self.cache/(key+'.json'))
            if source.suffix.lower()=='.rgb565':
                size=source.stat().st_size
                if size<=0 or size%FRAME or size>MAX_RAW:raise ValueError('原始文件须为完整1280×720 RGB565，且不超过8 GiB')
                fps=row['raw_fps']
                if type(fps) not in (int,float) or not math.isfinite(fps) or not 1<=fps<=60:raise ValueError('原始RGB565需要有效源帧率1–60')
                frames=size//FRAME;entry=self._entry(key,str(source),frames,float(fps) if frames>1 else 1.,row['name'],frames==1)
            elif cached and Path(cached['file']).is_file() and Path(cached['file']).stat().st_size==cached['frames']*FRAME:entry=cached
            else:
                if not shutil.which('ffmpeg') or not shutil.which('ffprobe'):raise ValueError('RK缺少ffmpeg/ffprobe，不能准备该格式')
                probe=subprocess.run(['ffprobe','-v','error','-protocol_whitelist','file,pipe','-show_streams','-show_format','-of','json',str(source)],capture_output=True,timeout=12,check=True)
                info=media_info(json.loads(probe.stdout),source.suffix.lower() in IMAGES)
                if self.cancelled.is_set():raise ValueError('准备已取消')
                if shutil.disk_usage(self.cache).free<info['estimate']+RESERVE:raise ValueError('转换完整素材后不足2 GiB剩余空间，未截短或删除原素材')
                if part.exists():raise ValueError('发现同名未完成缓存，请检查后再准备')
                filters='scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2:color=black,setsar=1'
                if not info['still']:filters='fps='+str(info['fps'])+','+filters
                command=['ffmpeg','-nostdin','-hide_banner','-loglevel','error','-protocol_whitelist','file,pipe','-threads','1','-filter_threads','1','-i',str(source),'-an','-sn','-dn','-vf',filters]
                if info['still']:command+=['-frames:v','1']
                command+=['-pix_fmt','rgb565le','-f','rawvideo',str(part)]
                if shutil.which('nice'):command=['nice','-n','10']+command
                with log.open('wb') as error:
                    with self.lock:self.process=subprocess.Popen(command,stdout=subprocess.DEVNULL,stderr=error,start_new_session=True)
                    while self.process.poll() is None:
                        if self.cancelled.wait(.25):self.process.terminate();break
                        size=part.stat().st_size if part.exists() else 0
                        self._set(progress=min(99,int(100*size/max(1,info['estimate']))),message='转换完整素材中；不运行AI推理')
                        if size>MAX_RAW or shutil.disk_usage(self.cache).free<RESERVE:self.process.terminate();raise ValueError('已达到缓存或空间上限，转换未完成')
                    try:code=self.process.wait(timeout=5)
                    except subprocess.TimeoutExpired:self.process.kill();self.process.wait();raise ValueError('转换进程未正常停止')
                if self.cancelled.is_set():raise ValueError('准备已取消')
                if code or not part.exists():raise ValueError('转换失败：'+log.read_text(errors='replace')[-220:])
                size=part.stat().st_size
                if size<=0 or size%FRAME or size>MAX_RAW:raise ValueError('转换结果不完整')
                if tuple(row['signature'])!=signature(source):raise ValueError('准备期间原素材发生变化')
                target=self.cache/(key+'.rgb565');part.replace(target)
                entry=self._entry(key,str(target),size//FRAME,info['fps'],row['name'],info['still'])
            if self.cancelled.is_set():raise ValueError('准备已取消')
            write(self.cache/(key+'.json'),entry)
            with self.lock:self.finished=entry;self.job.update(state='READY',message='完整素材已就绪',progress=100)
        except Exception as e:
            self._set(state='CANCELLED' if self.cancelled.is_set() else 'ERROR',message=str(e),progress=0)
        finally:
            with self.lock:
                if self.process and self.process.poll() is None:
                    self.process.terminate()
                    try:self.process.wait(timeout=3)
                    except subprocess.TimeoutExpired:self.process.kill();self.process.wait()
                self.process=None
            if part.exists():part.unlink() # only this newly-generated partial cache
    def _entry(self,key,file,frames,fps,name,still):
        cfg=self.cache/(key+'.config.json');write(cfg,{'scene':'usb_'+key,'width':1280,'height':720,'loop_frames':0 if still else frames,'rois':{'LEFT':[],'STRAIGHT':[],'RIGHT':[]}})
        return dict(id='usb_'+key,file=file,frames=frames,fps=fps,config=str(cfg),name=name,still=still)
    def take_finished(self,accept=True):
        with self.lock:value=self.finished if accept else None;self.finished=None;self.selected=value or self.selected;return value
    def cancel(self):
        self.cancelled.set()
        if self.thread:self.thread.join(timeout=15)
        if self.thread and self.thread.is_alive():raise RuntimeError('素材准备尚未退出，拒绝启动竞争任务')
    def playlist(self,entry):
        scenes=[{k:entry[k] for k in ('id','file','config','fps')}]
        manifest=read(self.runtime/'all_player/scene_manifest.json',{'scenes':[]})
        for row in manifest['scenes']:
            file=self.runtime/'video/replay'/row['asset_root']/row['file'];cfg=self.runtime/'all_player/configs'/(row['id']+'.json')
            if file.is_file() and cfg.is_file() and file.stat().st_size==row['frames']*FRAME:scenes.append(dict(id=row['id'],file=str(file),config=str(cfg),fps=row['fps']))
        path=self.cache/'active_playlist.json';write(path,{'scenes':scenes});return path
