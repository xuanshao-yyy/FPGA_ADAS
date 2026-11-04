"""Linux host simulator fidelity check, never connects to a board."""
from pathlib import Path
import json
import numpy as np
from rknn.api import RKNN
import onnxruntime as ort

HERE=Path(__file__).resolve().parent
def check(label,rc):
    if rc:raise RuntimeError(f'{label}: {rc}')
if __name__=='__main__':
    raw=np.load(HERE/'probe_rgb.npy');reference=np.load(HERE/'probe_logits.npy')
    session=ort.InferenceSession(str(HERE/'light_rgb_128.onnx'),providers=['CPUExecutionProvider'])
    onnx=[]
    for image in raw:
        normalized=(image.astype(np.float32)/255-np.array([.485,.456,.406],np.float32))/np.array([.229,.224,.225],np.float32)
        onnx.append(session.run(None,{'images':normalized.transpose(2,0,1)[None]})[0][0])
    assert np.allclose(reference,onnx,atol=1e-4,rtol=1e-4)
    r=RKNN(verbose=False)
    try:
        check('config',r.config(target_platform='rk3568',quantized_dtype='w8a8',quantized_algorithm='kl_divergence',quantized_method='channel',mean_values=[[123.675,116.28,103.53]],std_values=[[58.395,57.12,57.375]]))
        check('load',r.load_onnx(model=str(HERE/'light_rgb_128.onnx')))
        check('build',r.build(do_quantization=True,dataset=str(HERE/'calibration_128.txt')))
        check('init host simulator',r.init_runtime())
        quantized=[]
        for image in raw:quantized.append(np.asarray(r.inference(inputs=[image[None]],data_format=['nhwc'])[0]).reshape(3))
        quantized=np.asarray(quantized)
        def probability(logits):
            p=np.exp(logits-logits.max(axis=1,keepdims=True));return p/p.sum(axis=1,keepdims=True)
        drift=np.max(np.abs(probability(reference)-probability(quantized)),axis=1)
        result={'scope':'Three development RGB565 ROI probes; host simulator only, not board accuracy/FPS',
                'pytorch_logits':reference.tolist(),'int8_logits':quantized.tolist(),
                'max_probability_drift_per_probe':drift.tolist(),
                'argmax_agreement':(reference.argmax(1)==quantized.argmax(1)).tolist(),
                'onnx_max_logit_error':float(np.max(np.abs(reference-np.asarray(onnx))))}
        (HERE.parent/'verification/color_simulator.json').write_text(json.dumps(result,indent=2))
        print(json.dumps(result),flush=True)
        if not all(result['argmax_agreement']) or float(drift.max())>.1:
            raise RuntimeError('INT8 color fidelity failed; inspect report instead of claiming success')
    finally:r.release()
