"""Bounded parking telemetry IPC. No video decoding, painting, or model in Python."""
import collections,copy,errno,json,math,os,socket,stat,threading,time
from pathlib import Path

FIELDS=('sweep_hz','confidence','person_yellow','person_red','vehicle_yellow','vehicle_red','yellow_y','red_y','confirm_s','hold_s','cooldown_s')
NAMES=('前视','右侧','左侧','后视')
CLASSES=('行人','骑行者','汽车','公交车','货车','自行车','摩托车')

def validate_rules(value):
    if not isinstance(value,dict) or set(value)-set(FIELDS)-{'schema','revision','enabled'}:raise ValueError('未知驻车阈值字段')
    if value.get('enabled',1) not in (0,1,False,True):raise ValueError('enabled 必须为0或1')
    out={'schema':1,'revision':str(value.get('revision',''))[:64],'enabled':int(bool(value.get('enabled',1)))}
    for key in FIELDS:
        v=value.get(key)
        if isinstance(v,bool) or not isinstance(v,(float,int)) or not math.isfinite(v):raise ValueError('阈值必须是有限数字：'+key)
        out[key]=float(v)
    if not (out['sweep_hz']==0 or 1<=out['sweep_hz']<=4) or not .05<=out['confidence']<=.9:raise ValueError('更新率0为不限，或1–4Hz；置信度0.05–0.9')
    for prefix in ('person','vehicle'):
        if not .001<=out[prefix+'_yellow']<out[prefix+'_red']<=.8:raise ValueError('红色面积阈值须大于黄色，范围0.1%–80%')
    if not 0<=out['yellow_y']<out['red_y']<=1:raise ValueError('红区起点须低于黄区起点（0–1）')
    if not .1<=out['confirm_s']<=3 or not .2<=out['hold_s']<=5 or not 2<=out['cooldown_s']<=120:raise ValueError('确认0.1–3秒、保持0.2–5秒、记录间隔2–120秒')
    return out

def finite(value):return isinstance(value,(int,float)) and not isinstance(value,bool) and math.isfinite(value)

class Store:
    def __init__(self,rules,events):
        self.rules=validate_rules(rules);self.events=events;self.lock=threading.RLock();self.active=False
        self.latest={};self.history=collections.deque(maxlen=32);self.seen=collections.OrderedDict();self.record_error=''
    def reset(self,active=False):
        with self.lock:self.active=active;self.latest={};self.history.clear();self.record_error=''
    def configure(self,rules):
        with self.lock:self.rules=validate_rules(rules);self.latest={};self.history.clear()
    def accept(self,value,mode,now=None):
        now=time.monotonic() if now is None else now
        if not isinstance(value,dict):raise ValueError('Invalid parking observation')
        session=value.get('session');seq=value.get('sequence');finished=value.get('finished_s')
        if not isinstance(session,str) or not 1<=len(session)<=80 or not isinstance(seq,int) or seq<1 or not finite(finished) or not -.2<=now-finished<=3:raise ValueError('Invalid parking clock/session')
        with self.lock:
            if not self.active:return
            if self.latest.get('session')==session and seq<=self.latest.get('sequence',0):return
            if self.latest.get('session')!=session:self.history.clear()
            data=copy.deepcopy(value)
            if data.get('ready'):
                for key in ('received_s','video_pts'):
                    if not finite(data.get(key)) or data[key]<0:raise ValueError('Invalid frame timing')
                if not -.2<=now-data['received_s']<=2:raise ValueError('Stale parking frame')
                channels=data.get('channels')
                if not isinstance(channels,list) or len(channels)!=4:raise ValueError('Expected four parking views')
                rules=validate_rules(data.get('rules'))
                if rules['revision']!=self.rules['revision']:
                    self.latest=dict(session=session,sequence=seq,finished_s=finished,ready=False,error='等待新阈值载入');return
                for ch,view in enumerate(channels):
                    if not isinstance(view,dict) or view.get('channel')!=ch or not isinstance(view.get('boxes'),list) or len(view['boxes'])>12:raise ValueError('Invalid camera or target count')
                    for b in view['boxes']:
                        if not isinstance(b,dict) or b.get('category') not in range(7) or b.get('level') not in range(3):raise ValueError('Invalid target')
                        xy=b.get('xyxy')
                        if not isinstance(xy,list) or len(xy)!=4 or not all(finite(x) and 0<=x<=1 for x in xy) or xy[2]<=xy[0] or xy[3]<=xy[1]:raise ValueError('Invalid target bounds')
                        for key in ('score','area','foot','pending_s','last_seen_s'):
                            if not finite(b.get(key)):raise ValueError('Invalid target metric')
                        if not 0<=b['score']<=1 or not 0<=b['area']<=1 or not 0<=b['foot']<=1:raise ValueError('Invalid target fraction')
                        b['recorded']=False
                # Validation is complete before any persistent alarm side effects.
                for ch,view in enumerate(channels):
                    for b in view['boxes']:
                        event=b.get('event',0)
                        if mode!='PARK' or not view.get('valid') or b['level']!=2 or not isinstance(event,int) or event<=0:continue
                        key='parking-'+session+'-'+str(event)
                        if key not in self.seen:
                            try:
                                self.events.add(dict(event_id=key,type='PARK_ALERT',severity='RED',channel=ch,view=NAMES[ch],target=CLASSES[b['category']],
                                    score=b['score'],area=b['area'],foot=b['foot'],frame_id=data.get('frame_id'),video_pts=data['video_pts'],rules=rules,
                                    reason='面积与红区条件满足并持续确认',state=NAMES[ch]+' · '+CLASSES[b['category']]+' · 红色告警'))
                                self.seen[key]=True;self.record_error=''
                                while len(self.seen)>1024:self.seen.popitem(last=False)
                            except Exception as error:self.record_error='告警记录失败：'+str(error)
                        b['recorded']=key in self.seen
                self.history.append(data)
            self.latest=data
    def snapshot(self,mode,since=0,session=''):
        now=time.monotonic()
        with self.lock:
            last=self.latest;age=now-last.get('finished_s',-1000);source_age=now-last.get('received_s',-1000)
            fresh=bool(self.active and mode=='PARK' and last.get('ready') and 0<=age<=2 and 0<=source_age<=2)
            return dict(active=self.active,fresh=fresh,age_s=age if last else None,latest=last,rules=dict(self.rules),
                history=[v for v in self.history if v['session']!=session or v['sequence']>since],
                record_error=self.record_error,note='图像面积阈值演示；QV03尚未提供每个子画面的独立源帧龄')

class Listener:
    def __init__(self,path,store,mode):self.path=Path(path);self.store=store;self.mode=mode;self.stop=threading.Event();self.sock=None;self.thread=None;self.error=''
    def start(self):
        if len(os.fsencode(self.path))>=104:raise RuntimeError('Parking IPC path too long')
        # Supervisor holds its state-directory service flock before calling start.
        # Remove only a socket proven to have no listener after an interrupted run.
        if self.path.exists():
            if not stat.S_ISSOCK(self.path.stat().st_mode):raise RuntimeError('Parking IPC path is not a socket')
            with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as probe:
                try:probe.connect(str(self.path))
                except OSError as error:
                    if error.errno!=errno.ECONNREFUSED:raise
                    self.path.unlink()
                else:raise RuntimeError('Parking telemetry listener already running')
        self.sock=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);self.sock.bind(str(self.path));os.chmod(self.path,0o600);self.sock.settimeout(.3)
        self.inode=self.path.stat().st_ino;self.thread=threading.Thread(target=self.run,name='parking-telemetry',daemon=True);self.thread.start()
    def run(self):
        while not self.stop.is_set():
            try:
                raw=self.sock.recv(65536)
                if len(raw)>60000:raise ValueError('Oversized parking telemetry')
                self.store.accept(json.loads(raw),self.mode());self.error=''
            except socket.timeout:continue
            except (ValueError,UnicodeError,KeyError,TypeError) as e:self.error=str(e)
            except OSError as e:
                if not self.stop.is_set():self.error=str(e)
                break
    def close(self):
        self.stop.set()
        if self.thread:self.thread.join(timeout=1)
        if self.sock:self.sock.close()
        try:
            if self.path.stat().st_ino==self.inode:self.path.unlink()
        except (FileNotFoundError,AttributeError):pass
