"""Shared RGB crop preprocessing and four-class color model (not a lane model)."""
from pathlib import Path

import cv2
import numpy as np
import torch
from torch import nn
from torchvision.models import mobilenet_v3_small

NAMES = ['red', 'yellow', 'green', 'off']
SIZE = 128
MEAN = np.array([.485, .456, .406], dtype=np.float32)
STD = np.array([.229, .224, .225], dtype=np.float32)


def color_id(value, source):
    value = value.strip().lower()
    if source == 'bdd' and value == 'none':
        return None  # Unknown annotation is not proof of an extinguished signal.
    if source == 'bosch':
        value = next((name for name in ('red', 'yellow', 'green', 'off') if value.startswith(name)), value)
    return NAMES.index(value) if value in NAMES else None


def crop_bounds(box, width, height, padding=.08):
    x1, y1, x2, y2 = map(float, box)
    if not all(np.isfinite(v) for v in (x1, y1, x2, y2)) or x2 <= x1 or y2 <= y1:
        raise ValueError('Non-finite or degenerate crop box')
    dx, dy = (x2 - x1) * padding, (y2 - y1) * padding
    out = (max(0, int(np.floor(x1 - dx))), max(0, int(np.floor(y1 - dy))),
           min(width, int(np.ceil(x2 + dx))), min(height, int(np.ceil(y2 + dy))))
    if out[2] <= out[0] or out[3] <= out[1]:
        raise ValueError('Crop lies outside image')
    return out


def letterbox_rgb(image, size=SIZE):
    height, width = image.shape[:2]
    if height < 1 or width < 1:
        raise ValueError('Empty color crop')
    scale = min(size / width, size / height)
    nw, nh = max(1, round(width * scale)), max(1, round(height * scale))
    resized = cv2.resize(image, (nw, nh), interpolation=cv2.INTER_AREA if scale < 1 else cv2.INTER_LINEAR)
    out = np.full((size, size, 3), 114, dtype=np.uint8)
    left, top = (size - nw) // 2, (size - nh) // 2
    out[top:top + nh, left:left + nw] = resized
    return out


def tensor_rgb(image):
    data = (letterbox_rgb(image).astype(np.float32) / 255 - MEAN) / STD
    return torch.from_numpy(np.ascontiguousarray(data.transpose(2, 0, 1)))


def build_model(pretrained=None):
    model = mobilenet_v3_small(weights=None)
    if pretrained is not None:
        model.load_state_dict(torch.load(Path(pretrained), map_location='cpu', weights_only=True))
    model.classifier[-1] = nn.Linear(model.classifier[-1].in_features, len(NAMES))
    return model


def load_model(checkpoint):
    record = torch.load(checkpoint, map_location='cpu', weights_only=False)
    if record['names'] != NAMES or record['input_size'] != SIZE:
        raise ValueError('Unexpected color model contract')
    model = build_model()
    model.load_state_dict(record['state_dict'])
    return model.eval(), record


def classification_metrics(targets, predictions):
    matrix = np.zeros((4, 5), dtype=np.int64)  # column 4 is abstention, counted as a miss.
    np.add.at(matrix, (np.asarray(targets, dtype=int), np.asarray(predictions, dtype=int)), 1)
    per_class = {}
    for i, name in enumerate(NAMES):
        tp = matrix[i, i]
        precision = float(tp / max(1, matrix[:, i].sum()))
        recall = float(tp / max(1, matrix[i].sum()))
        per_class[name] = {'support': int(matrix[i].sum()), 'precision': precision, 'recall': recall,
                           'f1': 2 * precision * recall / max(1e-12, precision + recall)}
    present = [v['f1'] for v in per_class.values() if v['support']]
    n = int(matrix.sum())
    return {'accuracy': float(np.trace(matrix[:, :4]) / max(1, n)),
            'macro_f1': float(np.mean(present)) if present else 0., 'per_class': per_class,
            'confusion': matrix.tolist(), 'prediction_columns': NAMES + ['unknown'],
            'coverage': float(matrix[:, :4].sum() / max(1, n)),
            'red_predicted_green': int(matrix[0, 2]), 'images': n}


def hsv_color(rgb):
    """CPU baseline port of classify_light used by automatic signal processing."""
    h, w = rgb.shape[:2]
    if w < 4 or h < 6:
        return 4
    if max(h, w) > 128:
        rgb = cv2.resize(rgb, (max(1, round(w * 128 / max(h, w))), max(1, round(h * 128 / max(h, w)))),
                         interpolation=cv2.INTER_AREA)
    hsv = cv2.cvtColor(rgb, cv2.COLOR_RGB2HSV)
    hue, saturation, value = cv2.split(hsv)
    valid = (saturation >= 100) & (value >= 120)
    selections = [((hue <= 10) | (hue >= 160)), ((hue >= 15) & (hue <= 35)), ((hue >= 40) & (hue <= 95))]
    areas = []
    for selection in selections:
        _, _, stats, _ = cv2.connectedComponentsWithStats((selection & valid).astype(np.uint8), connectivity=8)
        areas.append(max((int(s[cv2.CC_STAT_AREA]) for s in stats[1:]
                          if s[cv2.CC_STAT_WIDTH] >= 2 and s[cv2.CC_STAT_HEIGHT] >= 2
                          and s[cv2.CC_STAT_WIDTH] <= 3 * s[cv2.CC_STAT_HEIGHT]
                          and s[cv2.CC_STAT_HEIGHT] <= 3 * s[cv2.CC_STAT_WIDTH]), default=0))
    best = int(np.argmax(areas))
    minimum = max(4, int(np.ceil(rgb.shape[0] * rgb.shape[1] * .005)))
    area = areas[best]
    if area < minimum or any(v >= minimum for i, v in enumerate(areas) if i != best):
        return 4
    if area / max(1, sum(areas)) < .85 or area > rgb.shape[0] * rgb.shape[1] * .6:
        return 4
    return best  # HSV never claims off solely because no color is visible.
