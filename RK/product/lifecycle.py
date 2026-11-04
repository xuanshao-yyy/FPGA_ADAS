"""Foreground launch/reuse and graceful stop of this exact product directory."""
from pathlib import Path
import argparse,json,os,signal,socket,subprocess,sys,time,urllib.request
ROOT=Path(__file__).resolve().parents[1]

def identity(pid,root):
    """Reject foreign versions and reused PIDs before sending any signal."""
    proc=Path('/proc')/str(pid)
    try:
        if pid<=1 or (proc/'cwd').resolve()!=root.resolve():return None
        args=(proc/'cmdline').read_bytes().split(b'\0')
        if len(args)<2:return None
        script=Path(os.fsdecode(args[1]));script=script if script.is_absolute() else root/script
        if script.resolve()!=(root/'product/server.py').resolve():return None
        words=(proc/'stat').read_text().rsplit(')',1)[1].split()
        return None if words[0]=='Z' else words[19]
    except (OSError,ValueError,IndexError):return None

def request(args,path,body=None):
    token=(args.state/'access_token.txt').read_text().strip()
    req=urllib.request.Request(args.url+'/api/'+path,data=None if body is None else json.dumps(body).encode(),headers={'X-Product-Token':token,'Content-Type':'application/json'})
    with urllib.request.urlopen(req,timeout=2) as reply:return json.load(reply)

def options(argv,root):
    parser=argparse.ArgumentParser(add_help=False)
    parser.add_argument('--port',type=int,default=8080);parser.add_argument('--listen',default='0.0.0.0')
    parser.add_argument('--state',type=Path,default=root/'results/product_runtime')
    prefix=argv[:argv.index('--')] if '--' in argv else argv
    args,_=parser.parse_known_args(prefix)
    if not 1<=args.port<=65535:raise ValueError('Invalid port')
    args.state=(args.state if args.state.is_absolute() else root/args.state).resolve()
    host='127.0.0.1' if args.listen=='0.0.0.0' else args.listen
    args.url='http://'+host+':'+str(args.port);args.host=host;return args

def occupied(args):
    with socket.socket() as sock:
        sock.settimeout(.5);return sock.connect_ex((args.host,args.port))==0

def existing(args,root):
    try:status=request(args,'status');pid=int(status['service_pid'])
    except (OSError,ValueError,KeyError,TypeError):raise RuntimeError('端口 %s 已被其他服务占用，未停止任何进程。'%args.port)
    start=identity(pid,root)
    if start is None:raise RuntimeError('端口 %s 的服务不属于当前产品目录，未停止任何进程。'%args.port)
    return status,pid,start

def stop_existing(args,root):
    _,pid,start=existing(args,root)
    if identity(pid,root)!=start:raise RuntimeError('服务身份已变化，未发送停止信号')
    os.kill(pid,signal.SIGINT)
    for _ in range(180):
        if identity(pid,root)!=start:print('产品服务已正常停止，可以重新启动。');return 0
        time.sleep(.25)
    raise RuntimeError('原服务尚未退出，请保留日志；没有强制终止其他进程。')

def reuse(args,root):
    status,pid,_=existing(args,root)
    if status.get('mode')=='FAULT':
        print('检测到本目录服务处于故障状态，正在请求恢复行驶界面……',flush=True)
        request(args,'mode',{'mode':'DRIVE'})
        for _ in range(80):
            status=request(args,'status')
            if status.get('mode')=='DRIVE' and status.get('drive_pid') and not status.get('error'):break
            time.sleep(.2)
        else:raise RuntimeError('恢复尚未完成：'+str(status.get('error') or status.get('mode'))+'；可先 bash stop_product.sh 再启动。')
    print('产品已经运行，PID=%s，模式=%s。复用已有服务，不再重复启动。'%(pid,status.get('mode','UNKNOWN')))
    print('本次启动参数未重新应用；需要重启时先执行 bash stop_product.sh。')
    # If a window manager helper is installed, activate only a window belonging
    # to the native PID returned by our authenticated, directory-checked service.
    targets={str(p) for p in (status.get('drive_pid'),status.get('panorama',{}).get('pid')) if p}
    if not targets:return 0
    try:
        rows=subprocess.check_output(['wmctrl','-lp'],universal_newlines=True,timeout=2).splitlines()
        for row in rows:
            cols=row.split(None,4)
            if len(cols)>=3 and cols[2] in targets:
                subprocess.run(['wmctrl','-ia',cols[0]],check=False,timeout=2);break
    except (OSError,subprocess.SubprocessError):pass
    return 0

def main(argv=None,root=ROOT):
    argv=list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] not in ('start','stop'):raise ValueError('Usage: lifecycle.py start|stop [product options]')
    action=argv.pop(0);args=options(argv,root)
    if action=='start' and any(v in ('-h','--help') for v in argv):os.execv(sys.executable,[sys.executable,str(root/'product/server.py'),*argv])
    if occupied(args):return stop_existing(args,root) if action=='stop' else reuse(args,root)
    if action=='stop':print('产品服务未运行。');return 0
    os.chdir(root);os.execv(sys.executable,[sys.executable,str(root/'product/server.py'),*argv])

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError) as exc:print('产品启动/停止提示：'+str(exc),file=sys.stderr);sys.exit(1)
