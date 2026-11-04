"""Use existing replay footage without reading scenario answer/reference labels."""
from pathlib import Path
import argparse
import json

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--suite',type=Path,required=True)
    parser.add_argument('--scene',choices=['lane_change','real_0061','real_0796','real_1077'],required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    folder=args.suite/'data'/args.scene
    if not folder.is_dir():folder=args.suite/args.scene
    manifest=json.loads((folder/'manifest.json').read_text(encoding='utf-8'))
    source=(folder/'ch0.rgb565').resolve();frames=int(manifest['frames']);fps=float(manifest['fps'])
    if frames<=0 or not 1<=fps<=120 or manifest['image_size']!=[1280,720] or source.stat().st_size!=frames*1280*720*2:
        raise ValueError('Invalid source metadata/size')
    config={'scene':args.scene,'width':1280,'height':720,'loop_frames':frames,'rois':{'LEFT':[],'STRAIGHT':[],'RIGHT':[]}}
    if args.output.exists():
        if json.loads(args.output.read_text(encoding='utf-8'))!=config:raise FileExistsError('Preserve altered scene config: '+str(args.output))
    else:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        with args.output.open('x',encoding='utf-8') as stream:json.dump(config,stream)
    print(source);print(fps);print(frames)

if __name__=='__main__':main()
