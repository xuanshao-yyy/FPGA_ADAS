"""Bounded fMP4 fragment cache. No video decoding or software encoding."""
import collections,os,select,threading,time

def boxes(data):
    at=0
    while at<len(data):
        if len(data)-at<8:raise ValueError('Truncated MP4 box')
        size=int.from_bytes(data[at:at+4],'big');kind=data[at+4:at+8];header=8
        if size==1:
            if len(data)-at<16:raise ValueError('Truncated large MP4 box')
            size=int.from_bytes(data[at+8:at+16],'big');header=16
        if size<header or at+size>len(data):raise ValueError('Invalid MP4 box length')
        yield kind,data[at+header:at+size];at+=size

def child(data,kind):return next((p for k,p in boxes(data) if k==kind),None)

def first_sync(moof,default_flags):
    """Inspect sample flags only; never decode/copy the H.264 payload."""
    traf=child(moof[8:],b'traf')
    if traf is None:return False
    tfhd=child(traf,b'tfhd')
    if tfhd is None or len(tfhd)<8:return False
    flags=int.from_bytes(tfhd[1:4],'big');at=8+(8 if flags&1 else 0)+(4 if flags&2 else 0)+(4 if flags&8 else 0)+(4 if flags&16 else 0)
    if flags&32:
        if len(tfhd)<at+4:return False
        default_flags=int.from_bytes(tfhd[at:at+4],'big')
    for kind,trun in boxes(traf):
        if kind!=b'trun' or len(trun)<8 or int.from_bytes(trun[4:8],'big')==0:continue
        flags=int.from_bytes(trun[1:4],'big');at=8+(4 if flags&1 else 0)
        if flags&4:
            if len(trun)<at+4:return False
            sample_flags=int.from_bytes(trun[at:at+4],'big')
        elif flags&1024:
            at+=(4 if flags&256 else 0)+(4 if flags&512 else 0)
            if len(trun)<at+4:return False
            sample_flags=int.from_bytes(trun[at:at+4],'big')
        else:sample_flags=default_flags
        return sample_flags is not None and not bool(sample_flags&0x10000)
    return False

class Fragments:
    def __init__(self):
        self.condition=threading.Condition();self.init=b'';self.ftyp=b'';self.codec='';self.items=collections.deque(maxlen=12);self.sequence=0;self.pending=b'';self.last=0;self.generation=0;self.sync=set();self.default_flags=None;self.pending_sync=False
    def reset(self):
        with self.condition:self.init=b'';self.ftyp=b'';self.codec='';self.items.clear();self.pending=b'';self.sequence=0;self.last=0;self.generation+=1;self.sync.clear();self.default_flags=None;self.condition.notify_all()
    def accept(self,box):
        typ=box[4:8]
        with self.condition:
            if typ==b'ftyp':self.ftyp=box
            elif typ==b'moov':
                if not self.ftyp:raise ValueError('moov before ftyp')
                self.init=self.ftyp+box
                i=box.find(b'avcC')
                if i<0 or len(box)<i+8:raise ValueError('H.264 avcC not found')
                self.codec='avc1.'+box[i+5:i+8].hex().upper()
                mvex=child(box[8:],b'mvex');trex=child(mvex,b'trex') if mvex else None
                self.default_flags=int.from_bytes(trex[20:24],'big') if trex and len(trex)>=24 else None
            elif typ==b'moof':self.pending=box;self.pending_sync=first_sync(box,self.default_flags)
            elif typ==b'mdat' and self.pending:
                if not self.init:raise ValueError('Fragment before initialization')
                self.sequence+=1;self.items.append((self.sequence,self.pending+box));self.pending=b'';self.last=time.monotonic()
                if self.pending_sync:self.sync.add(self.sequence)
                while sum(len(v) for _,v in self.items)>6*1024*1024:self.items.popleft()
                self.sync.intersection_update(n for n,_ in self.items)
            self.condition.notify_all()
    def info(self):
        with self.condition:return dict(ready=bool(self.items and self.sync and time.monotonic()-self.last<3),codec=self.codec,sequence=self.sequence,bytes=sum(len(v) for _,v in self.items),generation=self.generation)
    def evidence(self):
        with self.condition:
            if not self.init or not self.items or not self.sync or time.monotonic()-self.last>=3:return ()
            start=max(self.sync)
            return (self.init,)+tuple(v for n,v in self.items if n>=start)
    def stream(self,stopping):
        with self.condition:
            if not self.init or not self.items or not self.sync:return
            generation=self.generation;initial=self.init;cursor=max(self.sync)-1
        yield initial
        deadline=time.monotonic()+5
        while not stopping.is_set():
            with self.condition:
                if generation!=self.generation:return
                if self.items and cursor<self.items[0][0]-1:return # slow viewer reconnects at a recent GOP
                available=[(n,v) for n,v in self.items if n>cursor]
                if not available:
                    if time.monotonic()>deadline:return
                    self.condition.wait(.5);continue
            for n,chunk in available:yield chunk;cursor=n;deadline=time.monotonic()+5

class FifoReader:
    def __init__(self,path,fragments,stop):self.path=path;self.fragments=fragments;self.stop=stop;self.error=''
    def run(self):
        fd=os.open(self.path,os.O_RDWR|os.O_NONBLOCK);buffer=bytearray()
        try:
            while not self.stop.is_set():
                if not select.select([fd],[],[],.2)[0]:continue
                try:data=os.read(fd,262144)
                except BlockingIOError:continue
                if not data:continue
                buffer.extend(data)
                while len(buffer)>=8:
                    size=int.from_bytes(buffer[:4],'big');header=8
                    if size==1:
                        if len(buffer)<16:break
                        size=int.from_bytes(buffer[8:16],'big');header=16
                    if size<header or size>8*1024*1024:raise ValueError('Unbounded or invalid fMP4 box')
                    if len(buffer)<size:break
                    self.fragments.accept(bytes(buffer[:size]));del buffer[:size]
        except Exception as e:self.error=str(e)
        finally:os.close(fd)
