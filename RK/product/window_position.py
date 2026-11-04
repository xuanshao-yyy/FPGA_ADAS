"""Place the owned panorama on the primary monitor, once per mode transition."""
from pathlib import Path
import ctypes as C
import os,re,subprocess,time

def primary_monitor(text):
    rows=[]
    for line in text.splitlines():
        m=re.match(r'\s*\d+:\s+(\S+)\s+(\d+)(?:/\d+)?x(\d+)(?:/\d+)?([+-]\d+)([+-]\d+)\s',line)
        if m:rows.append(dict(name=m[1],width=int(m[2]),height=int(m[3]),x=int(m[4]),y=int(m[5])))
    if not rows:raise RuntimeError('Cannot determine an active display; preserve window position')
    return next((r for r in rows if '*' in r['name']),min(rows,key=lambda r:(r['x'],r['y'])))

def bounds(m):
    if m['width']<320 or m['height']<240:raise RuntimeError('Primary monitor is too small')
    return dict(x=m['x']+16,y=m['y']+48,width=min(1600,m['width']-42),height=min(650,m['height']-120))

def geometry(text):
    fields={name:int(re.search(pattern,text)[1]) for name,pattern in [('x',r'Absolute upper-left X:\s+(-?\d+)'),('y',r'Absolute upper-left Y:\s+(-?\d+)'),('width',r'Width:\s+(\d+)'),('height',r'Height:\s+(\d+)')]}
    return fields

def inside(g,m):return g['x']>=m['x'] and g['y']>=m['y'] and g['x']+g['width']<=m['x']+m['width'] and g['y']+g['height']<=m['y']+m['height']

def place_panorama(pid,executable):
    expected=Path(executable).resolve()
    if not isinstance(pid,int) or pid<=0 or (Path('/proc')/str(pid)/'exe').resolve()!=expected:raise RuntimeError('Panorama PID/executable does not belong to this runtime')
    env=dict(os.environ,DISPLAY=os.environ.get('DISPLAY',':0'))
    def query(*args):return subprocess.check_output(args,env=env,text=True,timeout=3)
    monitor=primary_monitor(query('xrandr','--listmonitors'))
    windows=re.findall(r'(0x[0-9a-f]+) "FPGA 360 Panorama \| QV04 \| RK3568"',query('xwininfo','-root','-tree'))
    if len(windows)!=1:raise RuntimeError('Cannot select exactly one panorama window')
    window=int(windows[0],16);before=geometry(query('xwininfo','-id',hex(window)));target=bounds(monitor)
    lib=C.CDLL('libX11.so.6');lib.XOpenDisplay.argtypes=[C.c_char_p];lib.XOpenDisplay.restype=C.c_void_p
    lib.XDefaultRootWindow.argtypes=[C.c_void_p];lib.XDefaultRootWindow.restype=C.c_ulong
    lib.XInternAtom.argtypes=[C.c_void_p,C.c_char_p,C.c_int];lib.XInternAtom.restype=C.c_ulong
    lib.XSendEvent.argtypes=[C.c_void_p,C.c_ulong,C.c_int,C.c_long,C.c_void_p]
    lib.XFlush.argtypes=[C.c_void_p];lib.XCloseDisplay.argtypes=[C.c_void_p]
    class Client(C.Structure):_fields_=[('type',C.c_int),('serial',C.c_ulong),('send_event',C.c_int),('display',C.c_void_p),('window',C.c_ulong),('message_type',C.c_ulong),('format',C.c_int),('data',C.c_long*5)]
    display=lib.XOpenDisplay(env['DISPLAY'].encode())
    if not display:raise RuntimeError('Cannot connect to the panorama desktop')
    try:
        root=lib.XDefaultRootWindow(display)
        def atom(name):return lib.XInternAtom(display,name.encode(),0)
        def send(name,data):
            event=Client(33,0,1,display,window,atom(name),32,(C.c_long*5)(*data))
            if not lib.XSendEvent(display,root,0,(1<<19)|(1<<20),C.byref(event)):raise RuntimeError('Window manager rejected panorama request')
            lib.XFlush(display)
        send('_NET_WM_STATE',[0,atom('_NET_WM_STATE_MAXIMIZED_HORZ'),atom('_NET_WM_STATE_MAXIMIZED_VERT'),2,0]);time.sleep(.1)
        send('_NET_MOVERESIZE_WINDOW',[1|0xf00|(2<<12),target['x'],target['y'],target['width'],target['height']])
        send('_NET_ACTIVE_WINDOW',[2,0,0,0,0])
        until=time.monotonic()+2
        while time.monotonic()<until:
            after=geometry(query('xwininfo','-id',hex(window)))
            if inside(after,monitor):return dict(window=hex(window),pid=pid,primary=monitor,before=before,after=after,inside_primary=True)
            time.sleep(.1)
        raise RuntimeError('Panorama is still outside the primary monitor')
    finally:lib.XCloseDisplay(display)
