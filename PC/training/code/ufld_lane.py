"""Standalone Windows inference for official UFLD-v2 CULane ResNet18 weights.

Architecture/decode adapted from cfzd/Ultra-Fast-Lane-Detection-v2 (MIT).
Original copyright/license: third_party/ufld_v2_reference/LICENSE.
This adapter avoids importing the official Linux DALI training pipeline.
No ImageNet weights are downloaded; the complete lane state is loaded strictly.
"""
from pathlib import Path
from project_paths import UFLD_WEIGHTS
import time

import cv2
import numpy as np
from PIL import Image
import torch
from torch import nn
from torchvision.models import resnet18
from torchvision import transforms


class ResNetFeatures(nn.Module):
    def __init__(self):
        super().__init__()
        base = resnet18(weights=None)
        for name in ['conv1', 'bn1', 'relu', 'maxpool', 'layer1', 'layer2', 'layer3', 'layer4']:
            setattr(self, name, getattr(base, name))

    def forward(self, x):
        x = self.maxpool(self.relu(self.bn1(self.conv1(x))))
        return self.layer4(self.layer3(self.layer2(self.layer1(x))))


class UFLDRes18(nn.Module):
    """Exact official CULane config: 320x1600, four row/column lane slots."""
    def __init__(self):
        super().__init__()
        self.model = ResNetFeatures()
        self.pool = nn.Conv2d(512, 8, 1)
        self.dims = [200 * 72 * 4, 100 * 81 * 4, 2 * 72 * 4, 2 * 81 * 4]
        self.cls = nn.Sequential(nn.LayerNorm(4000), nn.Linear(4000, 2048),
                                 nn.ReLU(), nn.Linear(2048, sum(self.dims)))

    def forward(self, x):
        values = self.cls(self.pool(self.model(x)).flatten(1))
        row, col, exists_row, exists_col = values.split(self.dims, dim=1)
        return dict(loc_row=row.reshape(-1, 200, 72, 4),
                    loc_col=col.reshape(-1, 100, 81, 4),
                    exist_row=exists_row.reshape(-1, 2, 72, 4),
                    exist_col=exists_col.reshape(-1, 2, 81, 4))


def decode_lanes(pred):
    """Official local-softmax decoding; normalized ORIGINAL image coordinates.

    Ego slots 1/2 use row anchors. Outer slots 0/3 use column anchors.
    No solid/dashed classification exists in these pretrained heads.
    """
    anchors = {'row': np.linspace(.42, 1, 72), 'col': np.linspace(0, 1, 81)}
    lanes = {}
    for axis, slots, fraction in [('row', [1, 2], .5), ('col', [0, 3], .25)]:
        logits = pred['loc_' + axis][0].float().cpu().numpy()
        exists = pred['exist_' + axis][0].argmax(0).cpu().numpy()
        indices = logits.argmax(0)
        grid, positions, _ = logits.shape
        for slot in slots:
            if np.sum(exists[:, slot]) <= positions * fraction:
                continue
            points = []
            for k in range(positions):
                if not exists[k, slot]: continue
                best = int(indices[k, slot])
                candidates = np.arange(max(0, best - 1), min(grid - 1, best + 1) + 1)
                local = logits[candidates, k, slot]
                probability = np.exp(local - local.max())
                coordinate = float((probability * candidates).sum() / probability.sum() + .5) / (grid - 1)
                point = (coordinate, anchors[axis][k]) if axis == 'row' else (anchors[axis][k], coordinate)
                if 0 <= point[0] <= 1 and 0 <= point[1] <= 1:
                    points.append(point)
            if len(points) >= 3:
                lanes[slot] = np.array(points, np.float32)
    return lanes


class UFLDLaneDetector:
    def __init__(self, weights=None, device='cuda:0'):
        self.device = torch.device(device)
        weights = weights or UFLD_WEIGHTS
        checkpoint = torch.load(weights, map_location='cpu', weights_only=True)
        state = checkpoint['model']
        state = {k.removeprefix('module.'): v for k, v in state.items()}
        self.net = UFLDRes18()
        self.net.load_state_dict(state, strict=True)
        self.parameters = sum(v.numel() for v in self.net.parameters())
        del checkpoint, state
        self.net.to(self.device).eval()
        # Match PIL official demo exactly: resize to int(320/.6), bottom crop320.
        self.transform = transforms.Compose([
            transforms.Resize((int(320 / .6), 1600)), transforms.ToTensor(),
            transforms.Normalize((.485, .456, .406), (.229, .224, .225))])
        self.last_ms = None

    @torch.inference_mode()
    def __call__(self, frame):
        rgb = Image.fromarray(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))
        tensor = self.transform(rgb)[:, -320:, :].unsqueeze(0).to(self.device)
        if self.device.type == 'cuda': torch.cuda.synchronize(self.device)
        begin = time.perf_counter()
        pred = self.net(tensor)
        if self.device.type == 'cuda': torch.cuda.synchronize(self.device)
        self.last_ms = (time.perf_counter() - begin) * 1000
        return decode_lanes(pred)


class LaneCurveFilter:
    """EMA per model slot. Short misses are disclosed predictions, never evidence.

    Large curve jumps reset; don't smooth lane changes into a fictitious lane.
    Slot identity is the model output slot, not a guaranteed physical lane ID.
    """
    def __init__(self, fps, alpha=.65, hold_seconds=.2):
        self.alpha, self.hold = alpha, round(fps * hold_seconds)
        self.history = {}
        self.ys = np.linspace(.45, .94, 60)

    def update(self, lanes):
        result = {}
        current = {}
        for slot, points in lanes.items():
            # Outer slots can be near-horizontal; preserve raw points for display.
            if slot not in (1, 2):
                result[slot] = dict(points=points, predicted=False)
                continue
            order = np.argsort(points[:, 1]); points = points[order]
            yy, index = np.unique(points[:, 1], return_index=True)
            xx = points[index, 0]
            ys = self.ys[(self.ys >= yy.min()) & (self.ys <= yy.max())]
            if len(ys) < 3: continue
            xs = np.interp(ys, yy, xx)
            previous = self.history.get(slot)
            if previous is not None and previous['missed'] <= self.hold:
                old = previous['points']
                overlap = (ys >= old[:, 1].min()) & (ys <= old[:, 1].max())
                if overlap.sum() >= 5:
                    old_x = np.interp(ys[overlap], old[:, 1], old[:, 0])
                    if np.median(np.abs(xs[overlap] - old_x)) < .08:
                        xs[overlap] = self.alpha * xs[overlap] + (1 - self.alpha) * old_x
            curve = np.column_stack([xs, ys]).astype(np.float32)
            current[slot] = dict(points=curve, missed=0)
            result[slot] = dict(points=curve, predicted=False)
        for slot, old in self.history.items():
            if slot not in current and old['missed'] < self.hold:
                current[slot] = dict(points=old['points'], missed=old['missed'] + 1)
                result[slot] = dict(points=old['points'], predicted=True)
        self.history = current
        return result


def lane_offset(lanes, ego_x=.5, reference_y=.85):
    """A visual offset candidate only; needs measured camera axis/calibration.

    Never compute from held predictions or extrapolate beyond observed support.
    Valid geometry is not proof of actual lane marking correctness.
    """
    if any(s not in lanes or lanes[s]['predicted'] for s in (1, 2)):
        return None
    xs = []
    for slot in (1, 2):
        p = lanes[slot]['points']; order = np.argsort(p[:, 1]); p = p[order]
        if not p[:, 1].min() <= reference_y <= p[:, 1].max(): return None
        xs.append(float(np.interp(reference_y, p[:, 1], p[:, 0])))
    left, right = xs
    if not left < ego_x < right or not .12 < right - left < .85: return None
    return float((ego_x - (left + right) / 2) / ((right - left) / 2))


def draw_curves(frame, lanes, title, offset=None):
    image = frame.copy(); h, w = image.shape[:2]
    if offset is not None:
        # Draw only shared observed range; preserve curve shape by interpolation.
        l, r = (lanes[s]['points'] for s in (1, 2))
        low, high = max(l[:, 1].min(), r[:, 1].min()), min(l[:, 1].max(), r[:, 1].max())
        ys = np.linspace(low, high, 60)
        left = np.column_stack([np.interp(ys, l[:, 1], l[:, 0]) * w, ys * h])
        right = np.column_stack([np.interp(ys, r[:, 1], r[:, 0]) * w, ys * h])
        overlay = image.copy()
        cv2.fillPoly(overlay, [np.int32(np.vstack([left, right[::-1]]))], (50, 170, 30))
        image = cv2.addWeighted(image, .82, overlay, .18, 0)
    for slot, entry in lanes.items():
        points = np.int32(entry['points'] * np.array([w, h]))
        color = (0, 190, 255) if entry['predicted'] else ((255, 230, 40) if slot in (1, 2) else (170, 90, 240))
        if entry['predicted']:
            for i in range(0, len(points) - 1, 3): cv2.line(image, tuple(points[i]), tuple(points[i+1]), color, 2)
        else: cv2.polylines(image, [points], False, color, 3, cv2.LINE_AA)
        if len(points):
            cv2.putText(image, f'L{slot}' + (' PRED' if entry['predicted'] else ''), tuple(points[-1]),
                        cv2.FONT_HERSHEY_SIMPLEX, .5, color, 1, cv2.LINE_AA)
    cv2.rectangle(image, (0, 0), (w, 90), (18, 22, 28), -1)
    info = 'OFFSET UNAVAILABLE' if offset is None else f'Offset candidate {offset:+.2f} lane half-width (+right)'
    for index, text in enumerate([title, info, 'Line type: NOT CLASSIFIED | uncalibrated prototype']):
        cv2.putText(image, text, (10, 23 + index * 27), cv2.FONT_HERSHEY_SIMPLEX, .54,
                    (230, 235, 235), 1, cv2.LINE_AA)
    return image
