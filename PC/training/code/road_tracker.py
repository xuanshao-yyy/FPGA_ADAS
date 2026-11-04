"""CPU ByteTrack adapter: input is original-image Nx6 xyxy/score/class detections.

The PC backend uses Ultralytics. On RK3568, use the official C++ ByteTrack
backend after RKNN postprocessing; do not install this PyTorch backend there.
"""
from types import SimpleNamespace

import numpy as np
from ultralytics.trackers.byte_tracker import BYTETracker


class DetectionBatch:
    """NumPy-only detection interface accepted by the PC ByteTrack backend."""

    def __init__(self, values):
        self.values = np.asarray(values, dtype=np.float32).reshape(-1, 6)

    def __len__(self):
        return len(self.values)

    def __getitem__(self, index):
        return DetectionBatch(self.values[index])

    @property
    def conf(self):
        return self.values[:, 4]

    @property
    def cls(self):
        return self.values[:, 5]

    @property
    def xywh(self):
        corners = self.values[:, :4]
        return np.column_stack(((corners[:, :2] + corners[:, 2:]) / 2,
                                corners[:, 2:] - corners[:, :2]))


class RoadTracker:
    """Class-separated ByteTrack with EMA display smoothing and short coasting.

    fps must describe the frames actually supplied to update(), not camera FPS
    if inference skips frames. Every video/scene needs a new instance.
    alpha closer to 1 follows new positions faster; smaller values add delay.
    """

    def __init__(self, fps=30, classes=8, alpha=.65):
        self.alpha = alpha
        self.hold_frames = min(2, max(0, round(fps * .08)))
        args = SimpleNamespace(track_high_thresh=.25, track_low_thresh=.1,
                               new_track_thresh=.25, track_buffer=max(1, round(fps * .5)),
                               match_thresh=.8, fuse_score=True)
        # Initialize all trackers before processing; IDs share the backend counter.
        self.trackers = [BYTETracker(args) for _ in range(classes)]
        self.display = {}

    def update(self, detections):
        """Return dicts with id, cls, score, box, predicted and measurement_box."""
        detections = DetectionBatch(detections)
        outputs, live_ids = [], set()
        for category, tracker in enumerate(self.trackers):
            subset = detections[detections.cls == category]
            tracker.update(subset)
            for track in tracker.tracked_stracks + tracker.lost_stracks:
                identity = track.track_id
                live_ids.add(identity)
                missed = tracker.frame_id - track.frame_id
                observed = missed == 0 and track.is_activated
                previous = self.display.get(identity)
                if not track.is_activated:
                    continue
                if observed and previous is None and track.tracklet_len < 1:
                    continue  # also confirm tracks born on the first frame
                if not observed and (previous is None or missed > self.hold_frames):
                    continue
                estimate = np.asarray(track.xyxy, dtype=np.float32)
                if previous is None or tracker.frame_id - previous['seen'] > self.hold_frames + 1:
                    box = estimate.copy()
                else:
                    box = self.alpha * estimate + (1 - self.alpha) * previous['box']
                seen = tracker.frame_id if observed else previous['seen']
                self.display[identity] = dict(box=box.copy(), seen=seen)
                measurement = subset.values[int(track.idx), :4].copy() if observed else None
                outputs.append(dict(id=identity, cls=category, score=float(track.score),
                                    box=box, predicted=not observed, measurement_box=measurement))
        self.display = {key: value for key, value in self.display.items() if key in live_ids}
        return outputs
