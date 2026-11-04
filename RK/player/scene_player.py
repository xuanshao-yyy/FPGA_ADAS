"""Scene routing only; references/scene names are never perception answers."""
from pathlib import Path
import argparse,hashlib,json,os,signal,subprocess,sys,time
HERE=Path(__file__).resolve().parent
RUNTIME=Path(os.environ.get('RUNTIME_DIR',str(HERE.parent))).resolve()
ASSETS=Path(os.environ.get('SCENE_ASSET_DIR',str(HERE))).resolve()

def load():
    m=json.loads((HERE/'scene_manifest.json').read_text(encoding='utf-8'));scenes=m['scenes']
    if not scenes or len({s['id'] for s in scenes})!=len(scenes):raise ValueError('Empty/duplicate scene list')
    return scenes

def roots():
    return {'player':ASSETS}

def source(scene):
    root=roots()[scene['asset_root']].resolve();p=(root/scene['file']).resolve()
    if root not in p.parents:raise ValueError('Source leaves declared root: '+str(p))
    return p

def check(scene,verify=False):
    p=source(scene);frames=int(scene['frames']);fps=float(scene['fps']);cfg=HERE/'configs'/(scene['id']+'.json')
    if frames<=0 or not 1<=fps<=120 or p.stat().st_size!=frames*1843200:raise ValueError('Invalid file/frame metadata: '+scene['id'])
    c=json.loads(cfg.read_text(encoding='utf-8'))
    if c['width']!=1280 or c['height']!=720 or c['loop_frames']!=(0 if scene['kind']=='static' else frames) or c['rois']!={'LEFT':[],'STRAIGHT':[],'RIGHT':[]}:raise ValueError('Wrong scene configuration: '+scene['id'])
    if verify:
        h=hashlib.sha256()
        with p.open('rb') as f:
            for b in iter(lambda:f.read(4194304),b''):h.update(b)
        if h.hexdigest()!=scene['sha256']:raise ValueError('SHA mismatch: '+scene['id'])
    return p,cfg

def command(scene,args,extra):
    p,cfg=check(scene);channels='0' if args.front_only else '0,1,2,3'
    return ['bash',str(RUNTIME/'run_competition_rules.sh'),'live',args.mode,
      '--playlist',str(args.output_root/'playlist_runtime.json'),'--scene-index',str(args.scene_index),'--file0',str(p),'--file-fps',str(scene['fps']),'--file-loop','on','--file-max-mib','512',
      '--signal-config',str(cfg),'--channels',channels,'--ai-channels',channels,
      '--preview-sink',os.environ.get('PREVIEW_BACKEND','xv'),'--shallow-weight','6','--deep-weight','3','--other-weight','1','--rx-batch-us','1000',
      '--output-root',str(args.output_root/(scene['id']+'_'+args.mode))]+extra

def main():
    p=argparse.ArgumentParser(description='NEXT cycles all configured scenes. Default: CH0 file + live FPGA CH1-3, all AI enabled.')
    p.add_argument('scene',nargs='?',default='intersection');p.add_argument('--mode',choices=['interactive','evidence','performance'],default='interactive')
    p.add_argument('--front-only',action='store_true');p.add_argument('--list',action='store_true');p.add_argument('--check',action='store_true');p.add_argument('--verify-data',action='store_true');p.add_argument('--dry-run',action='store_true');p.add_argument('--output-root',type=Path,default=HERE/'results')
    args,extra=p.parse_known_args();scenes=load()
    if args.list:
        for s in scenes:print(s['id']+' | '+s['kind']+' | '+str(s['fps'])+'Hz | '+s['notes'])
        return 0
    if args.check or args.verify_data:
        for s in scenes:check(s,args.verify_data);print('PASS '+s['id'])
        return 0
    selected=next((i for i,s in enumerate(scenes) if s['id']==args.scene),None)
    if selected is None:p.error('Unknown scene; use --list')
    args.scene_index=selected
    if args.dry_run:
        print(json.dumps({'command':command(scenes[selected],args,extra),'next':scenes[(selected+1)%len(scenes)]['id']},indent=2));return 0
    if not (RUNTIME/'run_competition_rules.sh').is_file():raise FileNotFoundError('Set RUNTIME_DIR to the existing verified RC9 runtime')
    args.output_root.mkdir(parents=True,exist_ok=True);child=None;stopping=0
    def stop(sig,frame):
        nonlocal stopping
        stopping=sig
        if child is not None and child.poll() is None:
            try:child.send_signal(sig)
            except ProcessLookupError:pass
    for sig in [signal.SIGINT,signal.SIGTERM,signal.SIGHUP]:signal.signal(sig,stop)
    # The C++ app owns NEXT/LIVE; launch exactly once and keep its window/models.
    playlist=[]
    for scene in scenes:
        raw,cfg=check(scene);playlist.append({'id':scene['id'],'file':str(raw),'config':str(cfg),'fps':scene['fps']})
    (args.output_root/'playlist_runtime.json').write_text(json.dumps({'scenes':playlist},indent=2)+'\n',encoding='utf-8')
    s=scenes[selected];argv=command(s,args,extra)
    record={'event':'START','unix_s':time.time(),'initial_scene':s['id'],'switching':'in_process_NEXT_LIVE','args':argv}
    with (args.output_root/'scene_history.jsonl').open('a',encoding='utf-8') as f:f.write(json.dumps(record)+'\n')
    print('NEXT switches clips in this window; LIVE restores CH0 FPGA. EXIT/Ctrl+C stops.',flush=True)
    child=subprocess.Popen(argv,cwd=RUNTIME,env=os.environ|{'DISPLAY':os.environ.get('DISPLAY',':0')});code=child.wait();child=None
    with (args.output_root/'scene_history.jsonl').open('a',encoding='utf-8') as f:f.write(json.dumps({'event':'EXIT','unix_s':time.time(),'code':code})+'\n')
    return 128+stopping if stopping else code if code>=0 else 128-code

if __name__=='__main__':
    try:sys.exit(main())
    except Exception as exc:print('Scene player error: '+str(exc),file=sys.stderr);sys.exit(1)
