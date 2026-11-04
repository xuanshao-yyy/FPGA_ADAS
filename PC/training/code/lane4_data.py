"""BDD legacy poly2d -> four-class semantic centerline bands (not paint-area masks)."""
import math

import cv2
import numpy as np

HEIGHT, WIDTH = 384, 640
IGNORE = 255
NAMES = ['background', 'solid', 'dashed', 'crosswalk']
MARKINGS = {'lane/single white', 'lane/single yellow', 'lane/single other',
            'lane/double white', 'lane/double yellow', 'lane/double other'}
PALETTE = np.array([[0, 0, 0], [30, 220, 30], [255, 120, 30], [180, 40, 220]], np.uint8)


def geometry(width, height):
    scale = min(WIDTH / width, HEIGHT / height)
    resized_width, resized_height = round(width * scale), round(height * scale)
    return scale, (WIDTH - resized_width) // 2, (HEIGHT - resized_height) // 2, resized_width, resized_height


def sample_polyline(poly, scale=1.0, left=0, top=0):
    """Match official legacy Path codes: first MOVETO, L line, C consumes 3 vertices.

    Control-vertex/end-vertex tags within a consumed cubic are not separate commands.
    The official renderer can contain CCCL and LCCCL, so treating C as an
    outgoing command or closing the path silently changes the geometry.
    """
    if not isinstance(poly, list) or len(poly) < 2:
        raise ValueError('Missing/short polyline')
    if any(len(point) != 3 or point[2] not in {'L', 'C'} for point in poly):
        raise ValueError('Invalid vertex/type')
    vertices = np.asarray([point[:2] for point in poly], np.float64) * scale + [left, top]
    if not np.isfinite(vertices).all():
        raise ValueError('Non-finite vertices')
    result = [vertices[0]]
    index = 1
    while index < len(poly):
        if poly[index][2] == 'L':
            result.append(vertices[index])
            index += 1
        else:
            if index + 2 >= len(poly):
                raise ValueError('Incomplete cubic Bezier')
            p0, p1, p2, p3 = result[-1], vertices[index], vertices[index + 1], vertices[index + 2]
            length = np.linalg.norm(p1 - p0) + np.linalg.norm(p2 - p1) + np.linalg.norm(p3 - p2)
            steps = max(8, min(2048, math.ceil(length / 2)))
            t = np.linspace(0, 1, steps + 1)[1:, None]
            curve = (1-t)**3*p0 + 3*(1-t)**2*t*p1 + 3*(1-t)*t**2*p2 + t**3*p3
            result.extend(curve)
            index += 3
    return np.rint(np.asarray(result)).astype(np.int32)


def classify(annotation):
    category = annotation['category']
    attrs = annotation.get('attributes', {})
    if category == 'lane/crosswalk':
        return 3
    if category == 'lane/road curb':
        return 0  # explicit negative, not a drivable lane boundary
    if category in MARKINGS:
        if attrs.get('direction') == 'vertical':
            return 0  # transverse non-crosswalk markings are not longitudinal boundaries
        if attrs.get('direction') != 'parallel':
            return IGNORE
        return {'solid': 1, 'dashed': 2}.get(attrs.get('style'), IGNORE)
    return IGNORE


def make_mask(objects, width, height, thickness=3):
    scale, left, top, resized_width, resized_height = geometry(width, height)
    lanes = [a for a in objects if a.get('category', '').startswith('lane/')]
    target = np.full((HEIGHT, WIDTH), IGNORE, np.uint8)
    if not lanes:
        return target, {'no_lane_annotations': True, 'classes': [], 'conflict_pixels': 0}
    target[top:top+resized_height, left:left+resized_width] = 0
    classes = [np.zeros((HEIGHT, WIDTH), np.uint8) for _ in range(4)]
    unknown = np.zeros((HEIGHT, WIDTH), np.uint8)
    for annotation in lanes:
        points = sample_polyline(annotation.get('poly2d'), scale, left, top)
        cls = classify(annotation)
        canvas = unknown if cls == IGNORE else classes[cls]
        cv2.polylines(canvas, [points], False, 1, thickness, cv2.LINE_8)
    valid = target != IGNORE
    count = sum(mask.astype(np.uint16) for mask in classes[1:])
    for cls in range(1, 4):
        target[(classes[cls] > 0) & valid] = cls
    # Conflicting classes and unknown attributes do not become arbitrary labels.
    conflict = (count > 1) | ((classes[0] > 0) & (count > 0))
    target[(conflict | (unknown > 0)) & valid] = IGNORE
    return target, dict(no_lane_annotations=False, classes=[c for c in range(1, 4) if (target == c).any()],
                        conflict_pixels=int(conflict.sum()))


def overlay_labels(image, target):
    view = image.copy()
    colored = (target > 0) & (target < 4)
    view[colored] = (view[colored].astype(np.float32) * .35 + PALETTE[target[colored]] * .65).astype(np.uint8)
    ignored = target == IGNORE
    # Sparse gray hatching shows ignored regions without hiding the road.
    yy, xx = np.indices(target.shape)
    hatch = ignored & ((xx + yy) % 12 < 2)
    view[hatch] = (view[hatch] * .4 + np.array([160, 160, 160]) * .6).astype(np.uint8)
    return view
