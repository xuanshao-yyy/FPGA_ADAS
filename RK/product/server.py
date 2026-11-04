"""RK product supervisor + authenticated LAN dashboard, Python standard library.
No inference, decoding or encoding in the HTTP service. Hardware video is fMP4.
"""
from pathlib import Path
import argparse,collections,errno,shutil,hmac,http.server,json,os,queue,secrets,signal,socket,sqlite3,stat,subprocess,sys,threading,time,urllib.parse,urllib.request
from control import Control
from calibration import validate,bmp565
from media import Fragments,FifoReader
from parking import Store,Listener,validate_rules
from library import Library
from evidence import Evidence
from runtime_log import collect as collect_runtime_log
from process_tree import descendants,wait_exited
from startup_planes import launch_once as launch_startup_planes
from window_position import place_panorama

ROOT=Path(__file__).resolve().parents[1]
def atomic(path,value):
    path=Path(path);temp=path.with_suffix(path.suffix+'.tmp');temp.write_text(json.dumps(value,ensure_ascii=False),encoding='utf-8')
    # Linux rename is atomic while readers hold an old inode. Windows fixture
    # readers can temporarily deny replacement; retry without deleting state.
    for attempt in range(30):
        try:temp.replace(path);return
        except PermissionError:
            if os.name!='nt' or attempt==29:raise
            time.sleep(.01)
def read_json(path,default=None):
    try:return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError,ValueError):return default

def remove_optional(path):
    # Keep compatibility with board Python 3.7 (Path.unlink lacks missing_ok).
    try:path.unlink()
    except FileNotFoundError:pass

class Events:
    def __init__(self,root,cloud=''):
        self.path=root/'events.sqlite';self.cloud=cloud;self.db=sqlite3.connect(self.path,check_same_thread=False);self.lock=threading.Lock();self.offset=0;self.inode=None;self.capture=None
        self.db.execute('CREATE TABLE IF NOT EXISTS events(id TEXT PRIMARY KEY, wall REAL, data TEXT, uploaded INTEGER DEFAULT 0)');self.db.commit()
    def add(self,value):
        value=dict(value)
        if value.get('type')=='PARK_ALERT' and self.capture:
            value['image_id']=self.capture();value['image_note']='告警前最近完整编码片段，不是完整前后录像'
        value.setdefault('event_id',secrets.token_hex(12));value.setdefault('wall_s',time.time())
        with self.lock:
            self.db.execute('INSERT OR IGNORE INTO events(id,wall,data) VALUES(?,?,?)',(value['event_id'],value['wall_s'],json.dumps(value,ensure_ascii=False)))
            self.db.execute('DELETE FROM events WHERE id NOT IN(SELECT id FROM events ORDER BY wall DESC LIMIT 10000)');self.db.commit()
    def ingest(self,path):
        try:
            info=path.stat()
            if info.st_ino!=self.inode or info.st_size<self.offset:self.offset=0;self.inode=info.st_ino
            with path.open('rb') as file:
                file.seek(self.offset)
                for _ in range(100):
                    pos=file.tell();line=file.readline(16385)
                    if not line or not line.endswith(b'\n'):break
                    self.offset=file.tell()
                    try:self.add(json.loads(line))
                    except (ValueError,UnicodeError):continue
        except FileNotFoundError:pass
    def list(self):
        with self.lock:return [json.loads(row[0]) for row in self.db.execute('SELECT data FROM events ORDER BY wall DESC LIMIT 100')]
    def upload(self,stop):
        # Network is never used unless the operator configures --cloud-url.
        while not stop.wait(30):
            if not self.cloud:continue
            try:
                with self.lock:rows=self.db.execute('SELECT id,data FROM events WHERE uploaded=0 ORDER BY wall LIMIT 20').fetchall()
                if not rows:continue
                body=json.dumps([json.loads(row[1]) for row in rows]).encode()
                request=urllib.request.Request(self.cloud,data=body,headers={'Content-Type':'application/json','Idempotency-Key':rows[0][0]})
                with urllib.request.urlopen(request,timeout=3) as reply:
                    if not 200<=reply.status<300:continue
                with self.lock:self.db.executemany('UPDATE events SET uploaded=1 WHERE id=?',[(row[0],) for row in rows]);self.db.commit()
            except Exception:continue

def parking_stream_options(args):
    fps=getattr(args,'park_fps',30);bitrate=getattr(args,'park_bitrate',None)
    if fps not in (15,30):raise ValueError('Parking video FPS must be 15 or 30')
    if bitrate is None:bitrate=5000000 if fps==30 else 2500000
    if not isinstance(bitrate,int) or not 200000<=bitrate<=8000000:raise ValueError('Invalid parking video bitrate')
    return dict(fps=fps,bitrate=bitrate)

class Supervisor:
    def __init__(self,args,control_factory=Control):
        self.stream_options=parking_stream_options(args);self.args=args;self.root=Path(args.state).resolve();self.root.mkdir(parents=True,exist_ok=True)
        self.calibration=self.root/'risk_profile.json'
        if not self.calibration.exists():atomic(self.calibration,read_json(ROOT/'configs/product_risk.json'))
        self.token_path=self.root/'access_token.txt'
        if not self.token_path.exists():self.token_path.write_text(secrets.token_urlsafe(24));self.token_path.chmod(0o600)
        self.token=self.token_path.read_text().strip();self.stop=threading.Event();self.commands=queue.Queue(maxsize=4);self.lock=threading.RLock()
        self.status=dict(mode='STARTING',requested='DRIVE',fpga=None,error='',drive_running=False,parking_running=False)
        self.events=Events(self.root,args.cloud_url);self.fragments=Fragments();self.control=None;self.factory=control_factory;self.drive=None;self.encoder=None;self.reader=None;self.reader_stop=None;self.reader_thread=None;self.started=time.monotonic();self.last_heartbeat=0;self.viewers=threading.BoundedSemaphore(3)
        self.park_rules=self.root/'parking_rules.json';self.park_active=self.root/'parking_ai.active'
        if not self.park_rules.exists():atomic(self.park_rules,read_json(ROOT/'configs/parking_guard.json'))
        near=self.root/'drive_near_rules.json'
        if not near.exists() and (ROOT/'configs/drive_near.json').is_file():atomic(near,read_json(ROOT/'configs/drive_near.json'))
        self.parking=Store(read_json(self.park_rules),self.events)
        self.park_listener=Listener(self.root/'park.sock',self.parking,lambda:self.status['mode'])
        self.panorama=None;self.panorama_started=0;self.mode_hub=None;self.drive_started=0
        self.library=Library(ROOT,self.root);self.media_entry=None;self.pending_media=False;self.media_cancel_requested=False;self.last_log_dir=None
        self.evidence=Evidence(self.root,self.fragments);self.events.capture=self.evidence.capture_clip
    def update(self,**values):
        with self.lock:
            self.status.update(values)
            if {'mode','requested','phase','error'}.intersection(values):
                atomic(self.root/'display_mode.json',dict(mode=self.status['mode'],target=self.status['requested'],phase=self.status.get('phase','正在准备界面'),error=self.status['error']))
    def phase(self,text):self.update(phase=text)
    def start_mode_hub(self):
        if self.args.web_only:return
        if self.mode_hub and self.mode_hub.poll() is None:return
        remove_optional(self.root/'hub_ready.json')
        self.mode_hub=self.launch([str(ROOT/'rk_mode_hub'),'--state',str(self.root)],'mode_hub')
        until=time.monotonic()+3
        while not self.stop.is_set() and time.monotonic()<until:
            if self.mode_hub.poll() is not None:raise RuntimeError('模式控制页启动失败，请查看 mode_hub.log')
            if read_json(self.root/'hub_ready.json',{}).get('pid')==self.mode_hub.pid:return
            self.stop.wait(.02)
        raise RuntimeError('模式控制页未就绪，保留原窗口')
    def stop_mode_hub(self):
        if self.mode_hub:self.stop_process(self.mode_hub);self.mode_hub=None
    def wait_drive_ready(self):
        if self.args.web_only:return
        until=time.monotonic()+15
        while not self.stop.is_set() and time.monotonic()<until:
            if not self.drive or self.drive.poll() is not None:raise RuntimeError('驾驶界面未能启动，请查看 drive.log')
            ready=read_json(self.root/'display_ready.json',{})
            pid=ready.get('pid')
            if isinstance(pid,int) and ready.get('monotonic_s',-1)>=self.drive_started:
                try:
                    if (Path('/proc')/str(pid)/'exe').resolve()==ROOT/'rk_signal_control':return
                except OSError:pass
            self.stop.wait(.1)
        raise RuntimeError('驾驶界面尚未提交新画面，请检查视频源和 drive.log')
    def command(self,mode):
        if mode not in ('DRIVE','PARK','PANORAMA'):raise ValueError('Unknown mode')
        if self.status['mode']=='SWITCHING':raise ValueError('模式正在切换，请等待完成')
        if self.pending_media:raise ValueError('素材准备中，请先取消准备或等待完成')
        if self.stop.is_set():raise ValueError('Service stopping')
        try:self.commands.put_nowait(mode)
        except queue.Full:raise ValueError('已有模式请求，稍后重试')
    def get_status(self):
        perception=read_json(self.root/'perception.json',{})
        age=time.monotonic()-perception.get('monotonic_s',-1)
        if not 0<=age<=2:perception={'stale':True,'channels':[]}
        with self.lock:result=dict(self.status)
        # The shell/desktop launcher may be a wrapper. Measure the C++ PID
        # published by the native bridge, not its waiting Python parent.
        running=self.drive and self.drive.poll() is None
        result.update(service_pid=os.getpid(),drive_pid=perception.get('pid') if running and not perception.get('stale') else None,drive_supervisor_pid=self.drive.pid if running else None,parking_pid=self.encoder.pid if self.encoder and self.encoder.poll() is None else None)
        result.update(parking_stream_target=self.stream_options,perception=perception,media=self.fragments.info(),uptime_s=round(time.monotonic()-self.started),cloud_configured=bool(self.args.cloud_url),board_tested=False)
        result.update(panorama_running=bool(self.panorama and self.panorama.poll() is None),panorama=self.panorama_state())
        result.update(library=self.library.state(),driver_ack=read_json(self.root/'driver_ack.json',{}),evidence_error=self.evidence.error,disk_free_bytes=shutil.disk_usage(self.root).free)
        result['note']='图像接近趋势不等同于真实碰撞时间；米数只在实测地面标定有效时显示。'
        return result
    def connect(self):
        if self.control is None:self.control=self.factory(bind=self.args.bind_a,peer=self.args.peer_a)
        r=self.control.query();self.update(fpga=r)
        if self.status['error'].startswith('FPGA 控制尚未连通:'):self.update(error='')
        return r
    def set_fpga(self,mode):
        if self.control is None:self.connect()
        r=self.control.set_mode(mode);self.update(fpga=r);return r
    def launch(self,command,name):
        log=(self.root/(name+'.log')).open('ab',buffering=0)
        try:return subprocess.Popen(command,cwd=ROOT,env=dict(os.environ,DISPLAY=os.environ.get('DISPLAY',':0'),LD_LIBRARY_PATH=str(ROOT/'lib')+':'+os.environ.get('LD_LIBRARY_PATH','')),stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        finally:log.close()
    def start_drive(self):
        if self.args.web_only:return
        if self.drive and self.drive.poll() is None:return
        command=['bash',str(ROOT/'start_surround.sh'),self.args.scene,'--product-dir',str(self.root),'--risk-profile',str(self.calibration),*self.args.drive_args]
        if self.media_entry:
            entry=self.media_entry;command+=['--playlist',str(self.library.playlist(entry)),'--scene-index','0','--file0',entry['file'],'--file-fps',str(entry['fps']),'--signal-config',entry['config'],'--file-max-mib','8192']
        remove_optional(self.root/'display_ready.json');self.drive_started=time.monotonic()
        self.drive=self.launch(command,'drive');self.update(drive_running=True)
        launch_startup_planes(self.root,self.drive,self.drive_started,self.stop,ROOT/'rk_signal_control',self.args.drive_args)
    def driver_command(self,name):
        keys={'LEFT':'a','STRAIGHT':'w','RIGHT':'d','LANE_LEFT':'j','LANE_RIGHT':'m','GO':' ','STOP':'s','CANCEL_LANE':'l','RECHECK':'c','PAUSE':'p','NEXT':'n','LIVE':'v'}
        if name not in keys:raise ValueError('Unknown driver command')
        with self.lock:
            status=self.get_status();pid=status.get('drive_pid');path=self.root/'driver_command.json'
            if status['mode']!='DRIVE' or not pid:raise ValueError('驾驶程序未就绪')
            if path.exists():raise ValueError('上一条操作正在处理')
            identity=secrets.token_hex(8);atomic(path,{'id':identity,'pid':pid,'key':keys[name],'sent_s':time.monotonic()})
            return {'queued':name,'id':identity}
    def runtime_log(self):
        data,self.last_log_dir=collect_runtime_log(ROOT,self.get_status().get('drive_pid'),self.last_log_dir)
        return data
    def open_media(self,key,fps=24):
        with self.lock:
            if self.pending_media or key not in self.library.files:raise ValueError('请刷新素材列表，或等待当前任务完成')
            self.pending_media=True;self.media_cancel_requested=False
            try:self.commands.put_nowait(('MEDIA',key,fps))
            except queue.Full:self.pending_media=False;raise ValueError('模式队列已满')
    def cancel_media(self):
        with self.lock:
            if not self.pending_media:raise ValueError('当前没有素材准备任务')
            self.media_cancel_requested=True;self.library.cancelled.set()
    def prepare_media(self,key,fps):
        was_drive=self.status['mode']=='DRIVE'
        self.update(mode='PREPARING',requested='DRIVE',error='')
        try:
            self.stop_panorama();self.stop_encoder();self.stop_process(self.drive);self.drive=None;self.update(drive_running=False)
            # DRIVE->local file keeps the same receiver mode; do not require
            # an absent FPGA to acknowledge again during offline front tests.
            # PARK/PANORAMA still require the existing verified transition.
            if not was_drive:self.set_fpga('DRIVE')
            remove_optional(self.root/'mode_request.json')
            if self.media_cancel_requested:
                self.pending_media=False;self.library._set(state='CANCELLED',message='素材准备已取消',progress=0);self.start_drive();self.update(mode='DRIVE');return
            self.library.begin(key,fps)
            if self.media_cancel_requested:self.library.cancelled.set()
        except Exception as e:
            self.pending_media=False;self.update(mode='FAULT',error='素材准备未启动: '+str(e))
    def poll_media(self):
        if self.status['mode']!='PREPARING':return
        job=self.library.state()
        if job['state'] not in ('READY','ERROR','CANCELLED'):return
        entry=self.library.take_finished(not self.media_cancel_requested)
        if entry:self.media_entry=entry
        self.pending_media=False
        try:
            self.start_drive();self.update(mode='DRIVE',error='' if entry else '素材未打开，已恢复原测试: '+job['message'])
            self.events.add({'type':'MEDIA_READY' if entry else 'MEDIA_FAILED','state':job['message'],'name':entry['name'] if entry else job.get('name','')})
        except Exception as e:self.update(mode='FAULT',error='恢复驾驶失败: '+str(e))
    def capture_evidence(self):
        if self.status['mode']=='DRIVE':
            status=self.get_status()
            if not status.get('drive_pid'):raise ValueError('驾驶画面尚未就绪')
            if not any(0<=v.get('rx_age_ms',-1)<750 for v in status.get('perception',{}).get('channels',[])):raise ValueError('没有新鲜接收画面可保存')
            (self.root/'evidence.request').touch();return {'queued':True}
        if self.status['mode']=='PARK':
            identity=self.evidence.capture_clip()
            if not identity:raise ValueError('暂无新编码片段或采样间隔未到，请稍后再试')
            self.events.add({'type':'MANUAL_EVIDENCE','image_id':identity,'state':'手动保存驻车编码片段'})
            return {'queued':True}
        raise ValueError('请先进入驾驶或驻车模式')
    def stop_process(self,child):
        if not child or child.poll() is not None:return
        owned=descendants(child.pid)
        for sig,timeout in [(signal.SIGINT,18),(signal.SIGTERM,5)]:
            try:os.killpg(child.pid,sig);child.wait(timeout=timeout);wait_exited(owned);return
            except ProcessLookupError:wait_exited(owned);return
            except subprocess.TimeoutExpired:continue
        # Do not launch a competing driver or kill unrelated processes.
        raise RuntimeError('Owned process did not stop; preserve logs and exit it before restarting')
    def panorama_state(self):
        value=read_json(self.root/'panorama_ready.json',{})
        now=time.monotonic()
        active=self.panorama and self.panorama.poll() is None
        valid=active and value.get('source')=='QV04' and value.get('monotonic_s',-1)>=self.panorama_started and 0<=now-value.get('received_s',-1)<.8 and value.get('complete',0)>0
        return dict(value,ready=bool(valid and value.get('ready')))
    def start_panorama(self):
        if self.args.web_only:raise RuntimeError('网页自检模式没有原生全景显示')
        self.stop_panorama();remove_optional(self.root/'panorama_ready.json');remove_optional(self.root/'panorama_stopping')
        self.panorama_started=time.monotonic()
        self.panorama=self.launch(['python3',str(ROOT/'scripts/desktop_session.py'),'--',str(ROOT/'rk_panorama_view'),
            '--bind',self.args.bind_a,'--peer',self.args.peer_a,'--state',str(self.root)],'panorama')
    def stop_panorama(self):
        if self.panorama:
            (self.root/'panorama_stopping').touch()
            self.stop_process(self.panorama);self.panorama=None
    def start_encoder(self):
        if self.args.web_only:raise RuntimeError('网页自检模式没有硬件编码器，不能进入驻车')
        if self.encoder and self.encoder.poll() is None:raise RuntimeError('驻车编码器仍在运行，拒绝重复启动')
        self.stop_encoder()
        fifo=self.root/'parking.fmp4.fifo'
        if fifo.exists():
            if not stat.S_ISFIFO(fifo.stat().st_mode):raise RuntimeError('Expected FIFO, refusing to overwrite file')
            fifo.unlink()
        os.mkfifo(fifo,0o600);self.fragments.reset();self.reader_stop=threading.Event();self.reader=FifoReader(fifo,self.fragments,self.reader_stop)
        self.reader_thread=threading.Thread(target=self.reader.run,name='parking-fragments',daemon=True);self.reader_thread.start()
        self.encoder=self.launch([str(ROOT/'rk_park_stream'),'--bind',self.args.bind_a,'--peer',self.args.peer_a,'--output',str(fifo),'--fps',str(self.stream_options['fps']),'--bitrate',str(self.stream_options['bitrate']),
            '--guard-model',str(getattr(self.args,'park_model',ROOT/'parking_model.rknn')),'--guard-socket',str(self.park_listener.path),
            '--guard-rules',str(self.park_rules),'--guard-active',str(self.park_active)],'parking')
    def stop_encoder(self):
        remove_optional(self.park_active);self.parking.reset(False)
        self.stop_process(self.encoder);self.encoder=None
        if self.reader_stop:self.reader_stop.set()
        if self.reader_thread:self.reader_thread.join(timeout=2)
        self.fragments.reset();self.update(parking_running=False)
    def configure_parking(self,value):
        rules=validate_rules(value);rules['revision']='parking-'+secrets.token_hex(8)
        with self.lock:atomic(self.park_rules,rules);self.parking.configure(rules)
        self.events.add({'type':'PARK_RULES_SAVED','revision':rules['revision']});return rules
    def transition(self,mode):
        if self.status['mode']==mode:return
        self.update(requested=mode,mode='SWITCHING',error='',phase='正在交接显示窗口')
        try:
            self.start_mode_hub()
            if mode=='PARK':
                if self.panorama:self.stop_panorama()
                self.phase('正在准备驻车硬件视频链路');self.start_encoder()
                self.phase('正在等待 FPGA 确认驻车');self.set_fpga('PARK')
                self.stop_process(self.drive);self.drive=None;self.update(drive_running=False)
                self.phase('正在等待驻车新画面')
                # Only release the parking NPU worker after the owned drive process exits.
                self.parking.reset(True);self.park_active.touch()
                until=time.monotonic()+7
                while not self.stop.is_set() and time.monotonic()<until:
                    if self.encoder.poll() is not None:raise RuntimeError('硬件编码器退出，请检查 parking.log')
                    if self.reader.error:raise RuntimeError(self.reader.error)
                    if self.fragments.info()['ready']:break
                    self.connect();self.stop.wait(.35)
                else:raise RuntimeError('FPGA 已确认驻车，但未收到可播放的四宫格编码视频')
                self.update(mode='PARK',parking_running=True,phase='驻车网页视频已就绪')
            elif mode=='PANORAMA':
                self.stop_process(self.drive);self.drive=None;self.update(drive_running=False)
                self.stop_encoder();self.phase('正在准备 360 显示');self.start_panorama();self.set_fpga('PANORAMA');self.phase('正在等待 360 新画面')
                until=time.monotonic()+8
                while not self.stop.is_set() and time.monotonic()<until:
                    if self.panorama.poll() is not None:raise RuntimeError('全景窗口退出，请检查 panorama.log')
                    if self.panorama_state()['ready']:break
                    self.connect();self.stop.wait(.15)
                else:raise RuntimeError('FPGA 已确认全景，但未收到完整 QV04 画面')
                self.phase('正在将 360 窗口放到主显示器')
                placed={} if self.args.web_only else place_panorama(self.panorama_state()['pid'],ROOT/'rk_panorama_view');atomic(self.root/'panorama_window.json',placed)
                self.update(mode='PANORAMA',parking_running=False,phase='360 新画面已就绪');self.stop_mode_hub()
            else:
                self.phase('正在恢复四路驾驶链路');self.set_fpga('DRIVE');self.stop_panorama();self.stop_encoder();self.start_drive();self.phase('正在等待驾驶新画面');self.wait_drive_ready();self.update(mode='DRIVE',parking_running=False,phase='驾驶画面已就绪');self.stop_mode_hub()
            self.events.add({'type':'MODE_APPLIED','mode':mode,'fpga':self.status['fpga']})
        except Exception as exc:
            error=str(exc);rollback=''
            # Always stop owned parking resources even if the FPGA cannot acknowledge rollback.
            try:
                self.phase('切换失败，正在恢复驾驶');self.stop_panorama();self.stop_encoder();self.set_fpga('DRIVE');self.start_drive();self.wait_drive_ready();self.update(mode='DRIVE',phase='已恢复驾驶');self.stop_mode_hub()
            except Exception as problem:rollback='; 恢复失败: '+str(problem);self.update(mode='FAULT')
            self.update(error=error+rollback,requested='DRIVE');self.events.add({'type':'MODE_FAILED','error':error+rollback})
    def loop(self):
        cloud=threading.Thread(target=self.events.upload,args=(self.stop,),daemon=True);cloud.start()
        try:
            if not self.args.web_only:self.park_listener.start()
            if not self.args.web_only:
                try:
                    r=self.connect()
                    if r['mode']!='DRIVE' or r['pending']:self.set_fpga('DRIVE')
                except Exception as e:self.update(error='FPGA 控制尚未连通: '+str(e))
                self.start_drive()
            self.update(mode='DRIVE')
            while not self.stop.wait(.25):
                try:mode=self.commands.get_nowait()
                except queue.Empty:mode=None
                native=self.root/'mode_request.json'
                if native.exists() and mode is None and self.status['mode']!='PREPARING':
                    data=read_json(native,{});native.unlink();mode=data.get('mode',mode)
                if isinstance(mode,tuple) and mode[0]=='MEDIA':self.prepare_media(mode[1],mode[2])
                elif mode in ('PARK','DRIVE','PANORAMA'):self.transition(mode)
                self.poll_media()
                if self.control and time.monotonic()-self.last_heartbeat>=1:
                    try:
                        reply=self.connect()
                        if self.status['mode']=='PARK' and reply['mode']!='PARK':raise RuntimeError('FPGA 已离开驻车，编码链路已停用')
                        if self.status['mode']=='PANORAMA' and reply['mode']!='PANORAMA':raise RuntimeError('FPGA 已离开全景，显示链路已停用')
                    except Exception as e:
                        self.update(error='FPGA 控制尚未连通: '+str(e),fpga=None)
                        if self.status['mode']=='PARK':self.update(mode='FAULT');self.stop_encoder()
                        if self.status['mode']=='PANORAMA':self.update(mode='FAULT');self.stop_panorama()
                    self.last_heartbeat=time.monotonic()
                if self.drive and self.drive.poll() is not None:self.update(drive_running=False,mode='FAULT',error='行驶程序已退出，请查看 drive.log');self.drive=None
                if self.encoder and self.encoder.poll() is not None:self.update(parking_running=False,mode='FAULT',error='驻车编码器已退出，请查看 parking.log');self.stop_encoder()
                if self.panorama and self.panorama.poll() is not None:
                    self.update(mode='FAULT',error='全景窗口意外退出，恢复行驶');self.stop_panorama();self.transition('DRIVE')
                self.events.ingest(self.root/'events.jsonl')
        finally:
            self.stop.set()
            remove_optional(self.park_active);self.parking.reset(False)
            try:
                if self.control:self.set_fpga('DRIVE')
            except Exception as e:print('FPGA restore pending (lease fallback):',e,flush=True)
            for cleanup in (self.stop_mode_hub,self.stop_panorama):
                try:cleanup()
                except Exception as e:print(e,flush=True)
            for child in (self.encoder,self.drive):
                try:self.stop_process(child)
                except Exception as e:print(e,flush=True)
            if self.reader_stop:self.reader_stop.set()
            if self.control:self.control.close()
            self.park_listener.close()
            self.library.cancel();self.evidence.close()

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args):pass
    @property
    def app(self):return self.server.app
    def send(self,code,value,kind='application/json'):
        body=json.dumps(value,ensure_ascii=False).encode() if kind=='application/json' else value
        self.send_response(code);self.send_header('Content-Type',kind);self.send_header('Content-Length',str(len(body)));self.send_header('Cache-Control','no-store');self.send_header('X-Content-Type-Options','nosniff');self.end_headers();self.wfile.write(body)
    def authorized(self):
        origin=self.headers.get('Origin')
        return (not origin or origin=='http://'+self.headers.get('Host','')) and hmac.compare_digest(self.headers.get('X-Product-Token',''),self.app.token)
    def do_GET(self):
        path=urllib.parse.urlsplit(self.path).path
        try:
            if path in ('/','/app.js','/style.css','/parking.js','/live.js','/workflow.js','/logo.png','/modes.js'):
                name={'/modes.js':'modes.js','/logo.png':'logo.png','/':'index.html','/app.js':'app.js','/parking.js':'parking.js','/live.js':'live.js','/workflow.js':'workflow.js','/style.css':'style.css'}[path];kind={'/modes.js':'text/javascript; charset=utf-8','/logo.png':'image/png','/':'text/html; charset=utf-8','/app.js':'text/javascript; charset=utf-8','/parking.js':'text/javascript; charset=utf-8','/live.js':'text/javascript; charset=utf-8','/workflow.js':'text/javascript; charset=utf-8','/style.css':'text/css; charset=utf-8'}[path]
                self.send(200,(ROOT/'product/web'/name).read_bytes(),kind);return
            if not self.authorized():self.send(401,{'error':'请使用启动终端提供的完整链接，或输入访问令牌'});return
            if path=='/api/status':self.send(200,self.app.get_status())
            elif path=='/api/runtime-log':self.send(200,self.app.runtime_log())
            elif path=='/api/events':self.send(200,self.app.evidence.decorate(self.app.events.list()))
            elif path=='/api/library':self.send(200,{'items':self.app.library.scan(),'job':self.app.library.state()})
            elif path=='/api/export':
                file,size=self.app.evidence.export(self.app.events.list(),self.app.get_status())
                try:
                    self.send_response(200);self.send_header('Content-Type','application/zip');self.send_header('Content-Length',str(size));self.send_header('Content-Disposition','attachment; filename=rk_evidence.zip');self.send_header('Cache-Control','no-store');self.end_headers()
                    shutil.copyfileobj(file,self.wfile,262144)
                finally:file.close()
            elif path.startswith('/api/evidence/'):
                data,kind=self.app.evidence.content(path.rsplit('/',1)[1]);self.send(200,data,kind)
            elif path=='/api/parking':
                query=urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
                self.send(200,self.app.parking.snapshot(self.app.status['mode'],int(query.get('since',['0'])[0]),query.get('session',[''])[0]))
            elif path=='/api/parking/rules':self.send(200,self.app.parking.snapshot(self.app.status['mode'])['rules'])
            elif path=='/api/calibration':self.send(200,read_json(self.app.calibration,{}))
            elif path=='/api/snapshot-meta':self.send(200,read_json(self.app.root/'snapshot_meta.json',{}))
            elif path.startswith('/api/snapshot/') and path.rsplit('/',1)[1] in ('0','1','2','3'):
                ch=int(path.rsplit('/',1)[1]);meta=read_json(self.app.root/'snapshot_meta.json',{})
                values=meta.get('channels',[])
                if ch>=len(values) or not values[ch].get('valid') or time.monotonic()-meta.get('monotonic_s',-1)>120:raise ValueError('请先获取新的标定画面')
                self.send(200,bmp565((self.app.root/('snapshot'+str(ch)+'.rgb565')).read_bytes()),'image/bmp')
            elif path=='/api/stream':self.video()
            else:self.send(404,{'error':'Not found'})
        except (BrokenPipeError,ConnectionResetError):pass
        except (ValueError,OSError) as e:self.send(400,{'error':str(e)})
    def do_POST(self):
        if not self.authorized():self.send(401,{'error':'访问令牌或来源无效'});return
        try:
            size=int(self.headers.get('Content-Length','0'))
            if not 0<size<=32768:raise ValueError('Invalid body size')
            body=json.loads(self.rfile.read(size))
            if self.path=='/api/mode':self.app.command(body['mode']);self.send(202,{'queued':body['mode']})
            elif self.path=='/api/driver-command':self.send(202,self.app.driver_command(body['command']))
            elif self.path=='/api/library/open':self.app.open_media(body['id'],body.get('raw_fps',24));self.send(202,{'queued':True})
            elif self.path=='/api/library/cancel':self.app.cancel_media();self.send(202,{'queued':True})
            elif self.path=='/api/evidence/capture':self.send(202,self.app.capture_evidence())
            elif self.path=='/api/parking/rules':self.send(200,self.app.configure_parking(body))
            elif self.path=='/api/snapshot':
                if not self.app.status['drive_running'] and not self.app.args.web_only:raise ValueError('标定快照需要行驶模式')
                (self.app.root/'snapshot.request').touch();self.send(202,{'requested':True})
            elif self.path=='/api/calibration':
                profile=validate(body);profile['id']='manual-'+time.strftime('%Y%m%d-%H%M%S')+'-'+secrets.token_hex(2)
                observation=read_json(self.app.root/'perception.json',{})
                profile['front_source']=observation.get('source_identity','')
                if profile['channels'][0]['verified'] and not profile['front_source']:raise ValueError('前视源尚未确认，请先启动行驶识别')
                atomic(self.app.calibration,profile);self.app.events.add({'type':'CALIBRATION_SAVED','id':profile['id']});self.send(200,profile)
            else:self.send(404,{'error':'Not found'})
        except (KeyError,ValueError,TypeError,OSError) as e:self.send(400,{'error':str(e)})
    def video(self):
        if not self.app.fragments.info()['ready']:self.send(503,{'error':'驻车视频尚未就绪'});return
        if not self.app.viewers.acquire(False):self.send(429,{'error':'最多同时三个视频客户端'});return
        try:
            self.connection.settimeout(4);self.connection.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1);self.send_response(200);self.send_header('Content-Type','video/mp4');self.send_header('Cache-Control','no-store');self.send_header('X-Accel-Buffering','no');self.send_header('Connection','close');self.end_headers();self.close_connection=True
            for data in self.app.fragments.stream(self.app.stop):self.wfile.write(data);self.wfile.flush()
        except (OSError,TimeoutError):pass
        finally:self.app.viewers.release()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--listen',default='0.0.0.0');parser.add_argument('--port',type=int,default=8080);parser.add_argument('--state',default=str(ROOT/'results/product_runtime'));parser.add_argument('--scene',default='intersection');parser.add_argument('--bind-a',default='192.168.10.1');parser.add_argument('--peer-a',default='192.168.10.2');parser.add_argument('--cloud-url',default='');parser.add_argument('--park-fps',type=int,choices=[15,30],default=30);parser.add_argument('--park-bitrate',type=int);parser.add_argument('--web-only',action='store_true');parser.add_argument('drive_args',nargs=argparse.REMAINDER);args=parser.parse_args()
    try:parking_stream_options(args)
    except ValueError as exc:parser.error(str(exc))
    args.park_model=str(ROOT/'parking_model.rknn')
    if args.drive_args[:1]==['--']:args.drive_args.pop(0)
    if args.cloud_url and urllib.parse.urlsplit(args.cloud_url).scheme not in ('http','https'):parser.error('cloud-url must use HTTP(S)')
    app=Supervisor(args)
    try:server=http.server.ThreadingHTTPServer((args.listen,args.port),Handler)
    except OSError as exc:
        if exc.errno==errno.EADDRINUSE:
            print('产品端口 %s 已被占用；请用 bash start_product.sh 复用已有服务，或先 bash stop_product.sh。'%args.port,file=sys.stderr)
            return 1
        raise
    server.daemon_threads=True;server.app=app
    # Singleton protects the two process owners and the fixed UDP control port.
    if os.name=='posix':
        import fcntl
        lock=(app.root/'service.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,lambda *_:app.stop.set())
    worker=threading.Thread(target=app.loop,name='product-supervisor');worker.start();server.timeout=.5
    print('LAN dashboard: http://<RK-LAN-IP>:'+str(args.port)+'/#token='+app.token,flush=True)
    try:
        while not app.stop.is_set():server.handle_request()
    finally:app.stop.set();server.server_close();worker.join(timeout=35)
if __name__=='__main__':sys.exit(main())
