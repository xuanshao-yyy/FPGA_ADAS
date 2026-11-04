"""Wait for owned launcher descendants, including separate desktop process groups."""
from pathlib import Path
import time

def identity(pid):
    try:
        data=(Path('/proc')/str(pid)/'stat').read_text();fields=data[data.rfind(')')+2:].split()
        return None if fields[0]=='Z' else (int(fields[1]),fields[19])
    except (OSError,ValueError,IndexError):return None

def descendants(pid):
    table={}
    for p in Path('/proc').iterdir():
        if p.name.isdigit():
            value=identity(int(p.name))
            if value is not None:table[int(p.name)]=value
    owned={pid};changed=True
    while changed:
        added={p for p,(parent,_) in table.items() if parent in owned}-owned
        changed=bool(added);owned.update(added)
    return {p:table[p][1] for p in owned if p!=pid and p in table}

def wait_exited(owned,timeout=18):
    end=time.monotonic()+timeout
    while True:
        active=[p for p,stamp in owned.items() if (value:=identity(p)) is not None and value[1]==stamp]
        if not active:return
        if time.monotonic()>=end:raise RuntimeError('Owned preview descendants still running: '+','.join(map(str,active)))
        time.sleep(.05)
