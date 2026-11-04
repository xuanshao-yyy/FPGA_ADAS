"""Prepare identical raw RGB frames for the independent RK3568 lane suite."""
import argparse
import hashlib
import json
from pathlib import Path

import cv2
import numpy as np


def prepare(source, output, start=35., seconds=2., stride=1, detector=None):
    if start < 0 or seconds <= 0 or stride < 1:
        raise ValueError('start>=0, seconds>0, stride>=1 required')
    capture = cv2.VideoCapture(str(source))
    if not capture.isOpened():
        raise RuntimeError(f'Cannot open {source}')
    fps = capture.get(cv2.CAP_PROP_FPS)
    total = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
    if fps <= 0:
        raise RuntimeError('Invalid source FPS')
    begin, end = round(start*fps), min(total, round((start+seconds)*fps))
    if begin >= end:
        raise RuntimeError('Empty requested clip')
    output.mkdir(parents=True, exist_ok=False)
    net=None
    if detector is not None:
        from ultralytics import YOLO
        net=YOLO(str(detector))
        expected=['person','rider','car','bus','truck','bicycle','motorcycle','traffic_light']
        if [net.names[i] for i in range(len(net.names))]!=expected:raise RuntimeError('Use this project\'s eight-class detector with the existing class order')
    capture.set(cv2.CAP_PROP_POS_FRAMES, begin)
    entries = []
    try:
        for index in range(begin, end):
            ok, image = capture.read()
            if not ok:
                raise RuntimeError(f'Unexpected end at frame {index}')
            if (index-begin) % stride:
                continue
            h, w = image.shape[:2]
            scale = min(640/w, 360/h)
            rw, rh = round(w*scale), round(h*scale)
            left, top = (640-rw)//2, (360-rh)//2
            frame = np.full((360,640,3),114,np.uint8)
            frame[top:top+rh,left:left+rw] = cv2.resize(image,(rw,rh))
            target = output/f'{len(entries):06}.rgb'
            target.write_bytes(cv2.cvtColor(frame,cv2.COLOR_BGR2RGB).tobytes())
            if net is not None:
                blocked=np.zeros((360,640),np.uint8)
                result=net.predict(frame,imgsz=640,conf=.25,verbose=False)[0]
                for box,cls in zip(result.boxes.xyxy.cpu().numpy(),result.boxes.cls.cpu().numpy()):
                    if int(cls) not in range(7):continue
                    x0,y0,x1,y1=box
                    x0,y0=max(0,int(np.floor(x0))),max(0,int(np.floor(y0)))
                    x1,y1=min(640,int(np.ceil(x1))),min(360,int(np.ceil(y1)))
                    blocked[y0:y1,x0:x1]=255
                (output/f'{len(entries):06}.blocked').write_bytes(blocked.tobytes())
            entries.append((index,index/fps,target.name))
    finally:
        capture.release()
    (output/'index.csv').write_text('source_frame,source_seconds,file\n'+''.join(
        f'{i},{t:.9f},{name}\n' for i,t,name in entries),encoding='utf-8')
    metadata = dict(source=str(source.resolve()), width=640,height=360,source_fps=fps,
        selected_frames=len(entries),start_frame=begin,end_frame_exclusive=end,stride=stride,
        frame_format='uint8 RGB HWC raw, no header', frame_period_seconds=stride/fps,
        original_width=w,original_height=h,content=[left,top,rw,rh], ground_truth_available=False,
        occlusion_masks=int(net is not None),occlusion_detector_sha256=hashlib.sha256(detector.read_bytes()).hexdigest() if net is not None else '',
        occlusion_scope='Predicted eight-class boxes0-6; detector inference excluded from lane-only board timings' if net is not None else '')
    (output/'meta.json').write_text(json.dumps(metadata,indent=2),encoding='utf-8')
    (output/'sha256.json').write_text(json.dumps({p.name:hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(output.iterdir()) if p.suffix in {'.rgb','.blocked'}},indent=2),encoding='utf-8')
    print(f'PREPARED {len(entries)} frames: {output}',flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--start',type=float,default=35.)
    parser.add_argument('--seconds',type=float,default=2.)
    parser.add_argument('--stride',type=int,default=1)
    parser.add_argument('--detector',type=Path,help='Optional existing eight-class YOLO best.pt for predicted occlusion masks')
    args=parser.parse_args()
    prepare(args.source,args.output,args.start,args.seconds,args.stride,args.detector)
