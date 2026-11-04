"""QC01/QA01 control, acknowledged at FPGA video frame boundary. Trusted LAN."""
import secrets,socket,struct,time,zlib

def request(session,seq,operation,mode):
    body=struct.pack('!4sIIBBH',b'QC01',session,seq,operation,mode,0)
    return body+struct.pack('!I',zlib.crc32(body))

def response(data):
    if len(data)!=28 or zlib.crc32(data[:24])!=struct.unpack('!I',data[24:])[0]:
        raise ValueError('Invalid acknowledgement length/CRC')
    magic,session,seq,mode,code,flags,frame,applied=struct.unpack('!4sIIBBHII',data[:24])
    if magic!=b'QA01' or mode>2 or code>4 or flags&~1:raise ValueError('Invalid acknowledgement fields')
    return dict(session=session,seq=seq,mode=('DRIVE','PARK','PANORAMA')[mode],code=code,pending=bool(flags&1),frame=frame,applied_seq=applied)

class Control:
    def __init__(self,bind='192.168.10.1',peer='192.168.10.2',port=6100,local_port=6101):
        self.sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        try:self.sock.bind((bind,local_port));self.sock.connect((peer,port));self.sock.settimeout(.22)
        except BaseException:self.sock.close();raise
        self.session=secrets.randbelow(0xfffffffe)+1;self.seq=0;self.claimed=False
    def close(self):self.sock.close()
    def exchange(self,op,mode=0):
        self.sock.send(request(self.session,self.seq,op,mode))
        until=time.monotonic()+.35
        while time.monotonic()<until:
            try:r=response(self.sock.recv(128))
            except ValueError:continue
            if r['session']==self.session and r['seq']==self.seq:return r
        raise TimeoutError('No matching FPGA acknowledgement')
    def query(self):
        r=self.exchange(0)
        if r['code']==2:raise RuntimeError('FPGA owned by another session; wait for its lease to expire')
        self.claimed=True;return r
    def set_mode(self,mode,timeout=3):
        if mode not in ('DRIVE','PARK','PANORAMA'):raise ValueError('Unknown mode')
        if not self.claimed:self.query()
        self.seq+=1
        if self.seq>=0xffffffff:raise RuntimeError('Restart control session before sequence wrap')
        until=time.monotonic()+timeout;last=None
        while time.monotonic()<until:
            try:
                last=self.exchange(1,('DRIVE','PARK','PANORAMA').index(mode))
                if last['code'] in (2,3,4):raise RuntimeError('FPGA rejected command: '+str(last['code']))
                if not last['pending'] and last['mode']==mode and last['applied_seq']==self.seq:return last
            except (socket.timeout,ConnectionRefusedError,TimeoutError):pass
            time.sleep(.06)
        raise TimeoutError('FPGA did not confirm applied mode; last='+str(last))
