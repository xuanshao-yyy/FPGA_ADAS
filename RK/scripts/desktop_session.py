"""Temporarily disable Xfce compositing, recover abandoned sessions safely.
No boot setting, clock, model or inference-scheduler change.
"""
from pathlib import Path
import fcntl,json,os,pwd,signal,subprocess,sys,time

def desktop():
    display=os.environ.get('DISPLAY',':0').split('.')[0]
    found=[]
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():continue
        try:
            if (proc/'comm').read_text().strip()!='xfwm4':continue
            env={}
            for item in (proc/'environ').read_bytes().split(b'\0'):
                if b'=' not in item:continue
                k,v=item.split(b'=',1)
                if k in [b'DISPLAY',b'DBUS_SESSION_BUS_ADDRESS',b'XDG_RUNTIME_DIR']:env[k.decode()]=v.decode()
            if env.get('DISPLAY','').split('.')[0]!=display:continue
            uid=proc.stat().st_uid;found.append((uid,env))
        except (FileNotFoundError,PermissionError):continue
    if len(found)!=1:raise RuntimeError('Cannot select one Xfce desktop')
    uid,env=found[0];prefix=[] if os.geteuid()==uid else ['runuser','-u',pwd.getpwuid(uid).pw_name,'--']
    cmd=prefix+['env']+[k+'='+v for k,v in env.items()]+['xfconf-query','-c','xfwm4','-p','/general/use_compositing']
    return uid,cmd

def query(cmd):
    value=subprocess.check_output(cmd,timeout=5,text=True).strip()
    if value not in ['true','false']:raise RuntimeError('Invalid compositor state')
    return value

def configure(cmd,value):
    subprocess.run(cmd+['-s',value],check=True,timeout=5)
    if query(cmd)!=value:raise RuntimeError('Desktop compositor did not accept '+value)

def boot_id():return Path('/proc/sys/kernel/random/boot_id').read_text().strip()
def lock_path(uid):return Path('/run/user')/str(uid)/'rk3568_preview_compositor.lock'

def active_previews():
    result=[]
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():continue
        try:
            if (proc/'comm').read_text().strip() in ['rk_signal_contr','rk_signal_control','rk_panorama_vie','rk_panorama_view']:result.append(proc.name)
        except (FileNotFoundError,PermissionError):continue
    return result

def save(path,state):
    # Atomic replacement avoids a truncated recovery JSON after interruption.
    temp=path.with_name(path.name+'.tmp.'+str(os.getpid()))
    with temp.open('w',encoding='utf-8') as stream:
        json.dump(state,stream,indent=2);stream.write('\n');stream.flush();os.fsync(stream.fileno())
    os.replace(temp,path)

def history(root,event,state):
    directory=root/'results/desktop_sessions';directory.mkdir(parents=True,exist_ok=True)
    save(directory/(str(time.time_ns())+'_'+str(os.getpid())+'_'+event+'.json'),state)

def validate(state,uid):
    if state.get('original') not in ['true','false']:raise RuntimeError('Invalid desktop recovery record; preserve it for diagnosis')
    display=os.environ.get('DISPLAY',':0').split('.')[0]
    if state.get('display',display).split('.')[0]!=display or state.get('desktop_uid',uid)!=uid:
        raise RuntimeError('Recovery record belongs to another desktop; not applying it')

def recover(root,state_path,uid,cmd,current,boot):
    if not state_path.exists():return current
    previous=json.loads(state_path.read_text());validate(previous,uid)
    if previous.get('restored') is not False:return current
    if previous.get('boot_id') and previous['boot_id']!=boot:
        # Xfce restarted; a previous boot must not override its current setting.
        event='new_boot_use_current'
    else:
        original=previous['original']
        if current!=original:configure(cmd,original)
        current=original;event='recovered_abandoned_session'
    history(root,event,dict(previous,recovery_event=event,recovery_unix_s=time.time(),current_boot_id=boot))
    print('Desktop session auto-recovery: '+event,flush=True)
    return current

def main():
    root=Path(__file__).resolve().parents[1];uid,cmd=desktop()
    fd=os.open(lock_path(uid),os.O_CREAT|os.O_RDWR|os.O_CLOEXEC|os.O_NOFOLLOW,0o600)
    if os.geteuid()==0:os.fchown(fd,uid,-1)
    with os.fdopen(fd,'w') as lock:
        try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:raise RuntimeError('Another preview session is running. Exit its window first.')
        if active_previews():raise RuntimeError('An RK test is still running. Exit its window first; desktop recovery was not applied.')
        state_path=root/'results/desktop_restore.json';state_path.parent.mkdir(parents=True,exist_ok=True)
        if sys.argv[1:]==['--restore']:
            if not state_path.exists():print('No desktop recovery record; nothing to restore.');return 0
            state=json.loads(state_path.read_text());validate(state,uid)
            configure(cmd,state['original']);state.update(restored=True,ended_unix_s=time.time())
            save(state_path,state);history(root,'manual_restore',state);print('Desktop compositor restored: True');return 0
        if len(sys.argv)<3 or sys.argv[1]!='--':raise RuntimeError('Usage: desktop_session.py -- command args OR --restore')
        boot=boot_id();old=recover(root,state_path,uid,cmd,query(cmd),boot)
        state={'original':old,'restored':False,'started_unix_s':time.time(),'display':os.environ.get('DISPLAY',':0'),
               'desktop_uid':uid,'boot_id':boot,'owner_pid':os.getpid()}
        save(state_path,state);child=None;code=1
        def forward(sig,frame):
            if child is not None and child.poll() is None:
                try:os.killpg(child.pid,sig)
                except ProcessLookupError:pass
        handlers={sig:signal.getsignal(sig) for sig in [signal.SIGINT,signal.SIGTERM,signal.SIGHUP]}
        for sig in handlers:signal.signal(sig,forward)
        try:
            configure(cmd,'false')
            print('Desktop compositor temporarily OFF; original='+old+'. Exit restores it. Model inference remains unlimited.',flush=True)
            child=subprocess.Popen(sys.argv[2:],env=dict(os.environ,RK_PREVIEW_SESSION_MANAGED='1'),start_new_session=True)
            code=child.wait()
        finally:
            try:
                try:
                    if child is not None and child.poll() is None:
                        os.killpg(child.pid,signal.SIGTERM)
                        try:child.wait(timeout=12)
                        except subprocess.TimeoutExpired:
                            os.killpg(child.pid,signal.SIGKILL);child.wait(timeout=3)
                finally:
                    try:configure(cmd,old);state['restored']=True
                    except Exception as exc:state['restore_error']=str(exc);raise
                    finally:
                        state['ended_unix_s']=time.time();save(state_path,state);history(root,'exit',state)
                        print('Desktop compositor restored='+str(state['restored']),flush=True)
            finally:
                for sig,handler in handlers.items():signal.signal(sig,handler)
        return code if code>=0 else 128-code

if __name__=='__main__':
    try:sys.exit(main())
    except Exception as exc:print('Desktop session error:',exc,file=sys.stderr);sys.exit(1)
