"""Official TwinLiteNet+ lane-only inference; binary candidates, no lane-style claims."""
import hashlib
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import cv2
import numpy as np
import torch

from project_paths import PROJECT_ROOT

INPUT_HEIGHT, INPUT_WIDTH = 384, 640


def load_lane_model(variant, device='cpu'):
    source = json.loads((PROJECT_ROOT / 'weights/twinlitenetplus/model_source.json').read_text(encoding='utf-8'))
    required = [f'weights/twinlitenetplus/{variant}.pth',
                'third_party/twinlitenetplus_reference/model/model.py',
                'third_party/twinlitenetplus_reference/model/config.py']
    for name in required:
        if hashlib.sha256((PROJECT_ROOT / name).read_bytes()).hexdigest() != source['files'][name]['sha256']:
            raise RuntimeError(f'Official file checksum changed: {name}')
    reference = PROJECT_ROOT / 'third_party/twinlitenetplus_reference'
    if 'model' in sys.modules:
        locations = list(getattr(sys.modules['model'], '__path__', []))
        if str(reference / 'model') not in locations:
            raise RuntimeError('Another module named model is loaded; run this probe independently.')
    if str(reference) not in sys.path:
        sys.path.insert(0, str(reference))
    from model.model import TwinLiteNetPlus
    full = TwinLiteNetPlus(SimpleNamespace(config=variant))
    checkpoint = torch.load(PROJECT_ROOT / required[0], map_location='cpu', weights_only=True)
    full.load_state_dict(checkpoint, strict=True)
    total = sum(p.numel() for p in full.parameters())

    class LaneOnly(torch.nn.Module):
        def __init__(self, original):
            super().__init__()
            self.encoder = original.encoder
            self.caam = original.caam
            self.conv_caam = original.conv_caam
            self.up_1_ll = original.up_1_ll
            self.up_2_ll = original.up_2_ll
            self.out_ll = original.out_ll

        def forward(self, image):
            encoded, inp1, inp2 = self.encoder(image)
            features = self.conv_caam(self.caam(encoded))
            features = self.up_1_ll(features, inp2)
            features = self.up_2_ll(features, inp1)
            return self.out_ll(features)

    lane = LaneOnly(full).eval().to(device)
    lane.full_model_parameters = total
    lane.variant = variant
    return lane


def prepare_rgb(image):
    """Fixed-size aspect-preserving letterbox, RGB /255 like the official demo."""
    height, width = image.shape[:2]
    scale = min(INPUT_WIDTH / width, INPUT_HEIGHT / height)
    resized_width, resized_height = round(width * scale), round(height * scale)
    left = (INPUT_WIDTH - resized_width) // 2
    top = (INPUT_HEIGHT - resized_height) // 2
    rgb = np.full((INPUT_HEIGHT, INPUT_WIDTH, 3), 114, dtype=np.uint8)
    rgb[top:top + resized_height, left:left + resized_width] = cv2.cvtColor(
        cv2.resize(image, (resized_width, resized_height), interpolation=cv2.INTER_LINEAR), cv2.COLOR_BGR2RGB)
    return rgb, (left, top, resized_width, resized_height)


def prepare_tensor(rgb):
    return torch.from_numpy(np.ascontiguousarray(rgb.transpose(2, 0, 1)))[None].float() / 255.0


def restore_mask(logits, image_shape, content):
    if logits.shape != (1, 2, INPUT_HEIGHT, INPUT_WIDTH) or not np.isfinite(logits).all():
        raise RuntimeError(f'Invalid lane logits: {logits.shape}')
    left, top, width, height = content
    original_height, original_width = image_shape[:2]
    scores = [cv2.resize(logits[0, channel, top:top + height, left:left + width],
                         (original_width, original_height), interpolation=cv2.INTER_LINEAR)
              for channel in range(2)]
    return scores[1] > scores[0]


def overlay(image, mask):
    result = image.copy()
    result[mask] = (result[mask].astype(np.float32) * .4 + np.array([255, 220, 0]) * .6).astype(np.uint8)
    return result
