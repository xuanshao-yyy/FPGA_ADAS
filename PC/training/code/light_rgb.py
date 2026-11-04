"""Three-color ROI experiment. Separate from the legacy four-class draft and YOLO."""
from functools import lru_cache
from pathlib import Path
import random

import cv2
import numpy as np
import torch
from torch import nn
from torch.utils.data import Dataset
from torchvision.models import mobilenet_v3_small

from light_color import crop_bounds, tensor_rgb

NAMES = ['red', 'yellow', 'green']
UNKNOWN = 3
INPUT_SIZE = 128
CONTRACT = {'input_size': INPUT_SIZE, 'channels': 'RGB', 'padding': .08,
            'resize': 'letterbox114', 'normalization': 'ImageNet mean/std',
            'scope': 'explicit red/yellow/green crops; no trained off/non-signal class'}


def build_rgb_model(pretrained=None):
    model = mobilenet_v3_small(weights=None)
    if pretrained is not None:
        model.load_state_dict(torch.load(pretrained, map_location='cpu', weights_only=True))
    model.classifier[-1] = nn.Linear(model.classifier[-1].in_features, 3)
    return model


def load_rgb_model(path, device='cpu'):
    checkpoint = torch.load(path, map_location='cpu', weights_only=True)
    if checkpoint.get('names') != NAMES or checkpoint.get('preprocessing') != CONTRACT:
        raise ValueError('Not a supported THREE-color ROI checkpoint; do not use a YOLO/lane/four-color PT')
    model = build_rgb_model()
    model.load_state_dict(checkpoint['state_dict'])
    return model.to(device).eval(), checkpoint


@lru_cache(maxsize=8)
def read_rgb(path):
    image = cv2.imdecode(np.fromfile(path, np.uint8), cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError(f'Cannot decode {path}')
    return cv2.cvtColor(image, cv2.COLOR_BGR2RGB)


class ColorCrops(Dataset):
    def __init__(self, rows, augment=False):
        self.rows = rows
        self.augment = augment

    def __len__(self):
        return len(self.rows)

    def __getitem__(self, index):
        row = self.rows[index]
        rgb = read_rgb(row['image'])
        h, w = rgb.shape[:2]
        x1, y1, x2, y2 = crop_bounds(row['box'], w, h)
        crop = rgb[y1:y2, x1:x2]
        if self.augment:
            # Same scalar on all RGB channels; no hue shifts or channel swapping.
            crop = np.clip(crop.astype(np.float32)*random.uniform(.85, 1.1), 0, 255).astype(np.uint8)
        return tensor_rgb(crop), NAMES.index(row['color'])


def reject_uncertain(probabilities, threshold=.6, margin=.15):
    values, ids = probabilities.topk(2, dim=1)
    valid = (values[:, 0] >= threshold) & (values[:, 0]-values[:, 1] >= margin)
    return torch.where(valid, ids[:, 0], torch.full_like(ids[:, 0], UNKNOWN))


def metrics(targets, predictions):
    matrix = np.zeros((3, 4), dtype=np.int64)
    targets, predictions = np.asarray(targets, dtype=int), np.asarray(predictions, dtype=int)
    if targets.shape != predictions.shape or np.any((targets < 0) | (targets > 2)) or np.any((predictions < 0) | (predictions > 3)):
        raise ValueError('Expected RGB truth 0..2 and predictions 0..3 (UNKNOWN)')
    np.add.at(matrix, (targets, predictions), 1)
    per_class = {}
    for i, name in enumerate(NAMES):
        tp = int(matrix[i, i])
        p = tp/max(1, int(matrix[:, i].sum()))
        r = tp/max(1, int(matrix[i].sum()))
        per_class[name] = {'support': int(matrix[i].sum()), 'precision': p, 'recall': r,
                           'f1': 2*p*r/max(1e-12, p+r)}
    present = [v['f1'] for v in per_class.values() if v['support']]
    return {'samples': int(matrix.sum()), 'accuracy': float(np.trace(matrix[:, :3])/max(1, matrix.sum())),
            'macro_f1': float(np.mean(present)) if present else 0., 'per_class': per_class,
            'coverage': float(matrix[:, :3].sum()/max(1, matrix.sum())),
            'red_predicted_green': int(matrix[0, 2]), 'confusion': matrix.tolist(),
            'columns': NAMES+['unknown']}
