"""Portable, material-free A runtime entry; no change to model or risk algorithms."""
from pathlib import Path
import argparse,json,os,subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser(description='Current A=YOLOv8+HSV live product. Import optional media from LAN library.')
    parser.add_argument('scene',nargs='?',default='live',choices=['live']);parser.add_argument('--dry-run',action='store_true')
    args,extra=parser.parse_known_args()
    square=ROOT/'models/yolov8n_bdd8_i8.rknn';front=ROOT/'models/yolov8n_bdd8_640x384_i8.rknn'
    for p in (square,front,ROOT/'models/small4_pad1_i8.rknn',ROOT/'rk_signal_control'):
        if not p.is_file():raise FileNotFoundError(str(p))
    command=['bash',str(ROOT/'run_competition_rules.sh'),'live','interactive',
        '--model',str(square),'--front-model',str(front),'--light-rgb-model','none',
        '--channels','0,1,2,3','--ai-channels','0,1,2,3','--preview-sink',os.environ.get('PREVIEW_BACKEND','xv'),
        '--shallow-weight','6','--deep-weight','3','--other-weight','1','--rx-batch-us','1000',
        '--lane-config',str(ROOT/'camera_wide.json'),'--signal-association','practical',
        '--lane-overlay','sparse','--view-layout','surround','--view-submit','batch',
        '--file-max-mib','8192','--output-root',str(ROOT/'results/current_drive'),*extra]
    if args.dry_run:print(json.dumps({'default_source':'FPGA_LIVE','model':'A YOLOv8+HSV','command':command},ensure_ascii=False,indent=2));return 0
    env=dict(os.environ,DISPLAY=os.environ.get('DISPLAY',':0'),RK_MODEL_LABEL='A YOLOv8+HSV')
    result=subprocess.run(command,cwd=ROOT,env=env);return result.returncode if result.returncode>=0 else 128-result.returncode
if __name__=='__main__':sys.exit(main())
