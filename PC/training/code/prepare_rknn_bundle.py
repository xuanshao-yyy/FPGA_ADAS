"""Prepare ONNX and calibration assets without changing source weights/code."""
import hashlib
import json
from pathlib import Path
import random
import shutil
import sys
import types

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / 'maintenance/rknn_prepare_20261002/python_deps'
sys.path.insert(0, str(DEPS))
BUNDLE = ROOT / 'deployment/rk3568_conversion'
sys.path.insert(0, str(BUNDLE))

import cv2
import numpy as np
import onnx
import onnxruntime as ort
import torch
from ultralytics import YOLO
from demo import letterbox


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def rockchip_head(self, features):
    outputs = []
    for index in range(self.nl):
        regression = self.cv2[index](features[index])
        classes = self.cv3[index](features[index]).sigmoid()
        outputs.extend([regression, classes, classes.sum(1, keepdim=True).clamp(0, 1)])
    return tuple(outputs)


def reconstruct(outputs):
    decoded = []
    for branch, stride in enumerate((8, 16, 32)):
        reg, cls = outputs[branch * 3:branch * 3 + 2]
        h, w = reg.shape[2:]
        probabilities = torch.from_numpy(reg).reshape(1, 4, 16, h, w).softmax(2)
        distances = (probabilities * torch.arange(16).reshape(1, 1, 16, 1, 1)).sum(2)[0].numpy()
        xx, yy = np.meshgrid(np.arange(w, dtype=np.float32) + .5,
                             np.arange(h, dtype=np.float32) + .5)
        xywh = np.stack((xx + (distances[2] - distances[0]) / 2,
                         yy + (distances[3] - distances[1]) / 2,
                         distances[0] + distances[2], distances[1] + distances[3])) * stride
        decoded.append(np.concatenate([xywh.reshape(1, 4, -1), cls.reshape(1, 8, -1)], axis=1))
    return np.concatenate(decoded, axis=2)


def main():
    torch.set_num_threads(4)
    for folder in ['models', 'calibration/images', 'samples', 'verification']:
        (BUNDLE / folder).mkdir(parents=True, exist_ok=True)
    source_pt = ROOT / 'runs/bdd8_full_n640/weights/best.pt'
    source_lane = ROOT / 'weights/yolop/yolop-640-640.onnx'
    original_hashes = {str(p.relative_to(ROOT)): sha256(p) for p in [source_pt, source_lane]}
    # Match the three-scale / 9-output contract in Rockchip's official exporter.
    model = YOLO(str(source_pt)).model.cpu().float().eval()
    head = model.model[-1]
    assert head.nc == 8 and head.reg_max == 16 and not head.end2end
    assert list(head.stride.tolist()) == [8, 16, 32]
    classes = [model.names[index] for index in range(8)]
    (BUNDLE / 'labels.txt').write_text('\n'.join(classes) + '\n', encoding='utf-8')
    shutil.copy2(source_pt, BUNDLE / 'models/bdd8_best.pt')

    # Six real-road videos only; exclude the rendered ADAS scene and in-car navigation screen.
    videos = sorted(p for p in (ROOT / 'video').glob('*.mp4')
                    if '111440' not in p.name and '112739' not in p.name)
    records, samples = [], []
    for video_index, video in enumerate(videos):
        capture = cv2.VideoCapture(str(video))
        count = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
        for sample_index, fraction in enumerate(np.linspace(.08, .92, 12)):
            frame_index = min(count - 1, round(count * fraction))
            capture.set(cv2.CAP_PROP_POS_FRAMES, frame_index)
            ok, frame = capture.read()
            if not ok:
                raise RuntimeError(f'Cannot read calibration frame {video}:{frame_index}')
            filename = f'calibration/images/video_{video_index:02}_{sample_index:02}.png'
            padded, _ = letterbox(frame)
            assert cv2.imwrite(str(BUNDLE / filename), padded)
            records.append(dict(file=filename, source=str(video.relative_to(ROOT)), frame=frame_index))
            if sample_index == 6:
                sample = f'samples/road_{video_index + 1:02}.jpg'
                assert cv2.imwrite(str(BUNDLE / sample), frame)
                samples.append(sample)
        capture.release()
    candidates = sorted((ROOT / 'datasets/bdd8/images/train').glob('*.jpg'))
    for index, image in enumerate(random.Random(42).sample(candidates, 128)):
        frame = cv2.imread(str(image))
        if frame is None:
            raise RuntimeError(f'Cannot read calibration image {image}')
        filename = f'calibration/images/bdd_{index:03}.png'
        padded, _ = letterbox(frame)
        assert cv2.imwrite(str(BUNDLE / filename), padded)
        records.append(dict(file=filename, source=str(image.relative_to(ROOT))))
    (BUNDLE / 'calibration/sources.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    (BUNDLE / 'calibration/dataset.txt').write_text(''.join(f"{r['file']}\n" for r in records), encoding='utf-8')

    inputs, native = [], []
    with torch.inference_mode():
        for sample in samples:
            image = cv2.imread(str(BUNDLE / sample))
            padded, _ = letterbox(image)
            tensor = cv2.cvtColor(padded, cv2.COLOR_BGR2RGB).transpose(2, 0, 1)[None].astype(np.float32) / 255
            inputs.append(tensor)
            native.append(model(torch.from_numpy(tensor))[0].numpy())
    # Bind only this in-memory instance, never installed Ultralytics or stored PT.
    head.forward = types.MethodType(rockchip_head, head)
    detection_path = BUNDLE / 'models/bdd8_yolov8n_640_rk.onnx'
    output_names = [f'{name}_s{stride}' for stride in (8, 16, 32)
                    for name in ('box', 'class', 'score_sum')]
    with torch.inference_mode():
        torch.onnx.export(model, torch.from_numpy(inputs[0]), str(detection_path),
                          input_names=['images'], output_names=output_names,
                          opset_version=12, dynamo=False, do_constant_folding=True)
    exported = onnx.load(str(detection_path))
    onnx.checker.check_model(exported)
    session = ort.InferenceSession(str(detection_path), providers=['CPUExecutionProvider'])
    expected_shapes = [[1, channel, 640 // stride, 640 // stride]
                       for stride in (8, 16, 32) for channel in (64, 8, 1)]
    assert [value.shape for value in session.get_outputs()] == expected_shapes
    verification = dict(detection=[], lane=[], rknn_conversion_verified=False, board_verified=False)
    for sample, tensor, expected in zip(samples, inputs, native):
        outputs = session.run(None, {'images': tensor})
        with torch.inference_mode():
            torch_outputs = [value.numpy() for value in model(torch.from_numpy(tensor))]
        for actual, reference in zip(outputs, torch_outputs):
            np.testing.assert_allclose(actual, reference, rtol=3e-4, atol=3e-4)
        decoded = reconstruct(outputs)
        box_error = float(np.max(np.abs(decoded[:, :4] - expected[:, :4])))
        score_error = float(np.max(np.abs(decoded[:, 4:] - expected[:, 4:])))
        assert box_error < .02 and score_error < 1e-4, (box_error, score_error)
        verification['detection'].append(dict(sample=sample, max_box_error_input_pixels=box_error,
                                             max_class_probability_error=score_error))
    del session
    print('Detection ONNX matches original PT on six road frames', flush=True)

    lane_model = onnx.load(str(source_lane))
    lane_input = lane_model.graph.input[0].name
    assert [d.dim_value for d in lane_model.graph.input[0].type.tensor_type.shape.dim] == [1, 3, 640, 640]
    lane_path = BUNDLE / 'models/yolop_lane_only_640.onnx'
    onnx.utils.extract_model(str(source_lane), str(lane_path), [lane_input], ['lane_line_seg'])
    onnx.checker.check_model(onnx.load(str(lane_path)))
    original = ort.InferenceSession(str(source_lane), providers=['CPUExecutionProvider'])
    extracted = ort.InferenceSession(str(lane_path), providers=['CPUExecutionProvider'])
    mean = np.array([.485, .456, .406], np.float32).reshape(1, 3, 1, 1)
    std = np.array([.229, .224, .225], np.float32).reshape(1, 3, 1, 1)
    for sample, tensor in zip(samples[:3], inputs[:3]):
        value = (tensor - mean) / std
        reference = original.run(['lane_line_seg'], {lane_input: value})[0]
        actual = extracted.run(None, {lane_input: value})[0]
        assert actual.shape == (1, 2, 640, 640)
        np.testing.assert_allclose(actual, reference, rtol=1e-5, atol=1e-5)
        verification['lane'].append(dict(sample=sample, max_absolute_error=float(np.max(np.abs(actual - reference)))))
    del original, extracted
    print('Lane-only ONNX matches original YOLOP lane output', flush=True)
    for relative, expected_hash in original_hashes.items():
        assert sha256(ROOT / relative) == expected_hash, f'Source weight changed: {relative}'
    verification['source_weights_unchanged'] = original_hashes
    verification['environment'] = dict(torch=torch.__version__, onnx=onnx.__version__,
                                       onnxruntime=ort.__version__)
    (BUNDLE / 'verification/onnx_validation.json').write_text(json.dumps(verification, indent=2), encoding='utf-8')
    calibration_files = [item['file'] for item in records]
    manifest = dict(target='rk3568', models={
        'detection': dict(onnx=str(detection_path.relative_to(BUNDLE).as_posix()),
                          output_stem='bdd8_yolov8n_640_rk3568', input_shape=[1, 3, 640, 640],
                          output_shapes=expected_shapes, classes=classes,
                          mean=[0, 0, 0], std=[255, 255, 255], calibration_files=calibration_files),
        'lane': dict(onnx=str(lane_path.relative_to(BUNDLE).as_posix()),
                     output_stem='yolop_lane_only_640_rk3568', input_shape=[1, 3, 640, 640],
                     output_shapes=[[1, 2, 640, 640]], mean=[123.675, 116.28, 103.53],
                     std=[58.395, 57.12, 57.375], calibration_files=calibration_files)},
                    source_weights=original_hashes)
    manifest['files'] = {p.relative_to(BUNDLE).as_posix(): sha256(p) for p in sorted(BUNDLE.rglob('*'))
                         if p.is_file() and p.name != 'manifest.json' and '__pycache__' not in p.parts}
    (BUNDLE / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(f'Bundle prepared: {BUNDLE}', flush=True)


if __name__ == '__main__':
    main()
