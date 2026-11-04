"""PC: check the exported raw DFL/class tensors against the unchanged PT."""
from pathlib import Path
import json
import numpy as np
import cv2
import torch
import onnxruntime as ort
from ultralytics import YOLO

HERE=Path(__file__).resolve().parent
if __name__=='__main__':
    torch.set_num_threads(2)
    model=YOLO(str(HERE/'traffic_yolo11n.pt')).model.float().eval()
    session=ort.InferenceSession(str(HERE/'traffic_yolo11n_640x384.onnx'),providers=['CPUExecutionProvider'])
    rows=[]
    for stem in ['red','green']:
        raw=np.fromfile(HERE.parent/'samples'/f'{stem}.rgb565',dtype='<u2').reshape(720,1280)
        r=(raw>>11)&31;g=(raw>>5)&63;b=raw&31
        rgb=np.stack([(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)],2).astype(np.uint8)
        image=np.full((384,640,3),114,np.uint8);image[12:372]=cv2.resize(rgb,(640,360))
        tensor=image.transpose(2,0,1)[None].astype(np.float32)/255
        with torch.inference_mode():reference=model(torch.from_numpy(tensor))[0].numpy()
        output=session.run(None,{'images':tensor});decoded=[]
        for branch in range(3):
            reg,cls,total=output[branch*3:branch*3+3];_,_,h,w=reg.shape
            assert cls.shape==(1,10,h,w) and total.shape==(1,1,h,w)
            weights=reg.reshape(1,4,16,h*w);weights=np.exp(weights-weights.max(2,keepdims=True));weights/=weights.sum(2,keepdims=True)
            distance=(weights*np.arange(16,dtype=np.float32)[None,None,:,None]).sum(2)
            yy,xx=np.mgrid[:h,:w];anchors=np.stack([xx.ravel()+.5,yy.ravel()+.5])[None].astype(np.float32)
            lo=anchors-distance[:,:2];hi=anchors+distance[:,2:]
            xywh=np.concatenate([(lo+hi)/2,hi-lo],1)*(8<<branch)
            decoded.append(np.concatenate([xywh,cls.reshape(1,10,-1)],1))
        prediction=np.concatenate(decoded,2)
        box_error=float(np.max(np.abs(reference[:,:4]-prediction[:,:4])))
        score_error=float(np.max(np.abs(reference[:,4:]-prediction[:,4:])))
        assert box_error<.05 and score_error<.0005
        rows.append({'probe':stem,'max_box_error_pixels':box_error,'max_score_error':score_error})
    (HERE.parent/'verification/onnx_parity.json').write_text(json.dumps({'source':'unchanged YOLO11 PT','scope':'FP32 export parity, not RKNN INT8 accuracy','probes':rows},indent=2))
    print(rows)
