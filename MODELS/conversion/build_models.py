"""Run with existing RKNN-Toolkit2 2.3.2; source models and old bundles stay intact."""
from pathlib import Path
import hashlib
import importlib.metadata
import json
from rknn.api import RKNN

HERE = Path(__file__).resolve().parent


def checked(label, status):
    if status != 0:
        raise RuntimeError(f'{label}: {status}')


if __name__ == '__main__':
    output = HERE.parent/'models'
    output.mkdir(exist_ok=True)
    manifest = output/'conversion.json'
    records = json.loads(manifest.read_text())['models'] if manifest.exists() else {}
    jobs = [(f'traffic_yolo11n_640x{h}', True) for h in (384,640)]
    jobs.append(('light_rgb_128',True))
    for stem, quantized in jobs:
        color = stem.startswith('light_rgb')
        target = output/f'{stem}_{"i8_kl256" if color else "i8"}.rknn'
        if target.exists():
            assert records[target.name]['sha256']==hashlib.sha256(target.read_bytes()).hexdigest()
            assert records[target.name]['source_onnx_sha256']==hashlib.sha256((HERE/f'{stem}.onnx').read_bytes()).hexdigest()
            print('VERIFIED existing new-run INT8',target,flush=True)
            continue
        model = RKNN(verbose=False)
        try:
            checked('config',model.config(target_platform='rk3568', optimization_level=3,
                quantized_dtype='w8a8',quantized_algorithm='kl_divergence' if color else 'normal',quantized_method='channel',
                mean_values=[[123.675,116.28,103.53] if color else [0,0,0]],
                std_values=[[58.395,57.12,57.375] if color else [255,255,255]]))
            checked('load_onnx',model.load_onnx(model=str(HERE/f'{stem}.onnx')))
            dataset = None
            if quantized:
                h = 128 if color else 384 if '384' in stem else 640
                dataset = HERE/f'calibration_{h}.txt'
                images = sorted(p for p in (HERE/f'calibration/{h}').iterdir() if p.suffix in ('.jpg','.png'))
                assert len(images) == (256 if color else 192)
                dataset.write_text(''.join(str(p.resolve())+'\n' for p in images))
            checked('build',model.build(do_quantization=quantized, dataset=str(dataset) if dataset else None))
            checked('export',model.export_rknn(str(target)))
            records[target.name] = {'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),
                                    'bytes':target.stat().st_size,'source_onnx_sha256':hashlib.sha256((HERE/f'{stem}.onnx').read_bytes()).hexdigest(),
                                    'quantized':quantized}
            (output/'conversion.json').write_text(json.dumps({'toolkit':importlib.metadata.version('rknn-toolkit2'),
                'target':'rk3568','models':records},indent=2))
            print('BUILT',target,flush=True)
        finally:
            model.release()
