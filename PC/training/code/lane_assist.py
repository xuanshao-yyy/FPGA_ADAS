"""Lane perception prototype; image coordinates, no metric calibration/control.

Only NumPy/OpenCV are used. Straight Hough seeds limit sharp-curve support.
The optional YOLOP mask supplies semantics; geometry still rejects horizontal
crosswalks, short fragments and implausible lane pairs. UNKNOWN is intentional.
"""
from dataclasses import dataclass

import cv2
import numpy as np


@dataclass
class Boundary:
    coeff: np.ndarray  # x(y), normalized image coordinates
    top: float
    bottom: float
    style: str = 'UNKNOWN'

    def x(self, y):
        return np.polyval(self.coeff, y)


class LaneMasks:
    def __init__(self, model=None):
        self.net = cv2.dnn.readNetFromONNX(str(model)) if model else None

    def __call__(self, frame):
        hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)
        white = cv2.inRange(hsv, (0, 0, 160), (180, 65, 255))
        yellow = cv2.inRange(hsv, (15, 65, 100), (40, 255, 255))
        paint = cv2.bitwise_or(white, yellow)
        if self.net is None:
            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            edges = cv2.Canny(cv2.GaussianBlur(gray, (5, 5), 0), 60, 150)
            mask = cv2.bitwise_and(edges, cv2.dilate(paint, np.ones((3, 3), np.uint8)))
        else:
            h, w = frame.shape[:2]
            scale = min(640 / w, 640 / h)
            rw, rh = round(w * scale), round(h * scale)
            left, top = (640 - rw) // 2, (640 - rh) // 2
            image = np.full((640, 640, 3), 114, np.uint8)
            image[top:top + rh, left:left + rw] = cv2.resize(frame, (rw, rh))
            rgb = image[:, :, ::-1].astype(np.float32) / 255
            rgb = (rgb - np.array([.485, .456, .406], np.float32)) / np.array([.229, .224, .225], np.float32)
            self.net.setInput(np.ascontiguousarray(rgb.transpose(2, 0, 1)[None]))
            # OpenCV 5 requests all outputs; this demo ignores YOLOP detections.
            outputs = dict(zip(self.net.getUnconnectedOutLayersNames(),
                               self.net.forward(self.net.getUnconnectedOutLayersNames())))
            scores = outputs['lane_line_seg'][0, :, top:top + rh, left:left + rw]
            scores = np.stack([cv2.resize(v, (w, h)) for v in scores])
            mask = ((scores[1] > scores[0]) & (scores[1] > .5)).astype(np.uint8) * 255
        return mask, paint


def marking_style(boundary, paint, blocked):
    """Conservative appearance heuristic, not a trained solid/dashed classifier.

    Sample ORIGINAL paint; never infer dash gaps from Hough's fitted line.
    Discard rows occluded by tracked vehicles. Insufficient evidence is UNKNOWN.
    """
    h, w = paint.shape
    ys = np.arange(round(boundary.top * h), min(round(boundary.bottom * h), h - 1))
    visible, hits = [], []
    for y in ys:
        x = round(float(boundary.x(y / h)) * w)
        if x < 5 or x >= w - 5 or blocked[y, max(0, x - 5):min(w, x + 6)].any():
            continue
        visible.append(y)
        hits.append(bool(paint[y, x - 5:x + 6].any()))
    if len(visible) < .13 * h or not hits:
        return 'UNKNOWN'
    coverage = np.mean(hits)
    if coverage >= .85:
        return 'SOLID_CANDIDATE'
    # Only accept multiple paint runs separated by substantial visible gaps.
    runs, gaps, count, previous = [], [], 0, None
    for y, hit in zip(visible, hits):
        if previous is not None and y > previous + 1:
            if count > 0: runs.append(count)
            elif count < 0: gaps.append(-count)
            count = 0  # occlusion never counts as a dash gap
        if hit:
            if count < 0: gaps.append(-count); count = 0
            count += 1
        else:
            if count > 0: runs.append(count); count = 0
            count -= 1
        previous = y
    if count > 0: runs.append(count)
    elif count < 0: gaps.append(-count)
    if .18 < coverage < .75 and sum(v > .025 * h for v in runs) >= 2 and sum(v > .025 * h for v in gaps) >= 2:
        return 'DASHED_CANDIDATE'
    return 'UNKNOWN'


def extract_boundaries(mask, paint, blocked, top=.55, bottom=.94):
    h, w = mask.shape
    roi = mask.copy()
    roi[:round(top * h)] = 0
    roi[round(bottom * h):] = 0
    roi[blocked > 0] = 0
    lines = cv2.HoughLinesP(roi, 1, np.pi / 180, 22,
                            minLineLength=round(.055 * h), maxLineGap=round(.035 * h))
    seeds = []
    if lines is None:
        return []
    for x1, y1, x2, y2 in lines.reshape(-1, 4):
        if abs(y2 - y1) < .045 * h or abs(y2 - y1) < .55 * abs(x2 - x1):
            continue  # crosswalks/stop lines are mostly horizontal
        a = ((x2 - x1) / w) / ((y2 - y1) / h)
        b = x1 / w - a * y1 / h
        xb = a * .90 + b
        if -.10 <= xb <= 1.10 and not any(abs(xb - old[0]) < .025 and abs(a - old[1]) < .15 for old in seeds):
            seeds.append((xb, a, b))
    boundaries = []
    for _, a, b in seeds:
        points = []
        for y in range(round(top * h), round(bottom * h), 3):
            x = round((a * y / h + b) * w)
            lo, hi = max(0, x - 9), min(w, x + 10)
            if hi <= lo:
                continue
            xs = np.flatnonzero(roi[y, lo:hi]) + lo
            if len(xs): points.append((y / h, np.median(xs) / w))
        if len(points) < 12:
            continue
        points = np.array(points)
        if np.ptp(points[:, 0]) < .13:
            continue
        coeff = np.polyfit(points[:, 0], points[:, 1], 1)
        error = np.abs(np.polyval(coeff, points[:, 0]) - points[:, 1])
        points = points[error < .015]
        if len(points) < 12 or np.ptp(points[:, 0]) < .13:
            continue
        coeff = np.polyfit(points[:, 0], points[:, 1], 1)
        boundary = Boundary(coeff, float(points[:, 0].min()), float(points[:, 0].max()))
        if boundary.bottom < .79:
            continue
        boundary.style = marking_style(boundary, paint, blocked)
        if not any(abs(boundary.x(.90) - old.x(.90)) < .035 for old in boundaries):
            boundaries.append(boundary)
    return sorted(boundaries, key=lambda v: v.x(.90))


def ego_pair(boundaries, ego_x=.5, reference_y=.90):
    left = [b for b in boundaries if b.x(reference_y) < ego_x]
    right = [b for b in boundaries if b.x(reference_y) > ego_x]
    if not left or not right:
        return None
    l, r = left[-1], right[0]
    lower_width = r.x(reference_y) - l.x(reference_y)
    upper_y = max(l.top, r.top, .65)
    upper_width = r.x(upper_y) - l.x(upper_y)
    # Require converging, long, bounded pair; no fabricated missing boundary.
    if not (.15 < lower_width < .85 and .015 < upper_width < lower_width * .92 and upper_y < .82):
        return None
    return l, r


class LaneAssist:
    def __init__(self, ego_x=.5, fps=10, confirm_seconds=.5, offset_threshold=.45):
        if not 0 < ego_x < 1 or fps <= 0:
            raise ValueError('ego_x must be in (0,1) and fps must be positive')
        self.ego_x, self.threshold = ego_x, offset_threshold
        self.confirm = max(1, round(fps * confirm_seconds))
        self.previous = None
        self.count = 0
        self.offset = None
        self.styles = [[], []]

    def update(self, boundaries, command='straight', force=False):
        pair = ego_pair(boundaries, self.ego_x)
        if pair is None:
            self.previous, self.offset, self.count, self.styles = None, None, 0, [[], []]
            return dict(state='UNKNOWN', offset=None, pair=None, ego_x=self.ego_x, left='UNKNOWN', right='UNKNOWN',
                        action='WAIT: LANE UNAVAILABLE', alarm=False, blinker=command)
        if self.previous is not None:
            if max(abs(a.x(.90) - b.x(.90)) for a, b in zip(pair, self.previous)) > .10:
                self.count, self.offset, self.styles = 0, None, [[], []]
            else:
                pair = tuple(Boundary(.65 * a.coeff + .35 * b.coeff, a.top, a.bottom, a.style)
                             for a, b in zip(pair, self.previous))
        self.previous = pair
        self.count += 1
        for index, boundary in enumerate(pair):
            self.styles[index].append(boundary.style)
            self.styles[index] = self.styles[index][-self.confirm:]
        stable_styles = [v[-1] if len(v) == self.confirm and len(set(v)) == 1 else 'UNKNOWN' for v in self.styles]
        l, r = (b.x(.90) for b in pair)
        raw_offset = (self.ego_x - (l + r) / 2) / ((r - l) / 2)
        self.offset = raw_offset if self.offset is None else .65 * raw_offset + .35 * self.offset
        state = 'CHECKING' if self.count < self.confirm else 'CENTERED'
        if state != 'CHECKING' and abs(self.offset) > self.threshold:
            state = 'DEVIATION RIGHT' if self.offset > 0 else 'DEVIATION LEFT'
        action, alarm = 'KEEP LANE', False
        if state == 'CHECKING':
            action = 'WAIT: CONFIRMING LANES'
        elif command in ('left', 'right'):
            style = stable_styles[0 if command == 'left' else 1]
            if style == 'SOLID_CANDIDATE':
                action = 'BLOCK: SOLID CANDIDATE'
                alarm = force
                if force: action = 'ALARM: FORCED SOLID CHANGE'
            elif style == 'DASHED_CANDIDATE':
                action = 'DASHED CANDIDATE: CHECK SIDE'
            else:
                action = 'WAIT: LINE TYPE UNKNOWN'
        elif state.startswith('DEVIATION'):
            action, alarm = 'OFFSET WARNING', True
        return dict(state=state, offset=float(self.offset), pair=pair, ego_x=self.ego_x, left=stable_styles[0],
                    right=stable_styles[1], action=action, alarm=alarm, blinker=command)


def draw_lanes(frame, mask, status, title):
    image = frame.copy()
    evidence = mask > 0
    image[evidence] = (.5 * image[evidence] + .5 * np.array([0, 150, 255])).astype(np.uint8)
    pair = status['pair']
    if pair is not None:
        h, w = image.shape[:2]
        ys = np.linspace(max(b.top for b in pair), min(b.bottom for b in pair), 50)
        points = [np.int32(np.column_stack([b.x(ys) * w, ys * h])) for b in pair]
        overlay = image.copy()
        cv2.fillPoly(overlay, [np.vstack([points[0], points[1][::-1]])], (50, 170, 30))
        image = cv2.addWeighted(image, .8, overlay, .2, 0)
        for line in points: cv2.polylines(image, [line], False, (255, 240, 60), 3)
        cv2.line(image, (round(status['ego_x'] * w), round(.86 * h)), (round(status['ego_x'] * w), round(.93 * h)), (255, 255, 255), 2)
    cv2.rectangle(image, (0, 0), (image.shape[1], 118), (20, 22, 26), -1)
    color = (40, 60, 255) if status['alarm'] else (100, 235, 235)
    offset = 'N/A' if status['offset'] is None else f"{status['offset']:+.2f} (lane half-width; +right)"
    for i, text in enumerate([title, f"{status['state']} | offset {offset}",
                              f"L: {status['left']}  R: {status['right']}",
                              f"{status['action']} | command: {status['blinker']}"]):
        cv2.putText(image, text, (10, 23 + i * 27), cv2.FONT_HERSHEY_SIMPLEX, .52, color, 1, cv2.LINE_AA)
    # Driver command display, not a steering actuator or inferred vehicle motion.
    h, w = image.shape[:2]
    start = (w // 2, h - 42)
    end = {'left': (w // 2 - 65, h - 42), 'right': (w // 2 + 65, h - 42),
           'straight': (w // 2, h - 100)}[status['blinker']]
    cv2.arrowedLine(image, start, end, color, 4, tipLength=.35)
    return image
