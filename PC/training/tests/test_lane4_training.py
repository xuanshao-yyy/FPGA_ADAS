"""Training checks: ignored pixels, foreground metrics and pretrained transfer."""
from pathlib import Path
from contextlib import redirect_stdout
import io
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'code'))
import numpy as np
import torch
from train_lane4 import segmentation_loss, metrics_from_confusion, create_model, check_pair_paths, checked_gradient_norm, duration
from twinlite_lane import load_lane_model
from run_lane4 import load_checkpoint


class Lane4TrainingTests(unittest.TestCase):
    def test_directory_index_checks_all_pairs_and_not_once_per_file(self):
        with tempfile.TemporaryDirectory() as temporary, redirect_stdout(io.StringIO()):
            base = Path(temporary)
            root = base/'dataset'
            (root/'masks').mkdir(parents=True)
            (base/'images').mkdir()
            rows = []
            for stem in ['a', 'b']:
                (base/f'images/{stem}.jpg').write_bytes(b'image')
                (root/f'masks/{stem}.png').write_bytes(b'mask')
                rows.append(dict(stem=stem, image=f'../images/{stem}.jpg', mask=f'masks/{stem}.png'))
            with patch('train_lane4.os.scandir', wraps=os.scandir) as scanner:
                check_pair_paths(root, rows, 'train')
                self.assertEqual(scanner.call_count, 2)
            # A missing later pair must still fail, and cached names do not survive a new call.
            (root/'masks/b.png').unlink()
            with self.assertRaisesRegex(RuntimeError, 'Missing dataset pair: b'):
                check_pair_paths(root, rows, 'train')

    def test_directory_named_like_a_mask_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary, redirect_stdout(io.StringIO()):
            root = Path(temporary)
            (root/'image.jpg').write_bytes(b'image')
            (root/'mask.png').mkdir()
            with self.assertRaisesRegex(RuntimeError, 'Missing dataset pair: a'):
                check_pair_paths(root, [dict(stem='a', image='image.jpg', mask='mask.png')], 'val')

    def test_ignored_logits_have_no_loss_or_gradient_effect(self):
        target = torch.tensor([[[0, 1], [2, 255]]])
        logits = torch.zeros(1, 4, 2, 2, requires_grad=True)
        loss = segmentation_loss(logits, target, torch.ones(4))
        modified = logits.detach().clone()
        modified[0, :, 1, 1] = torch.tensor([1000., -1000., 500., -500.])
        self.assertAlmostEqual(float(loss.detach()), float(segmentation_loss(modified, target, torch.ones(4))), places=6)
        loss.backward()
        self.assertTrue(torch.isfinite(logits.grad).all())
        self.assertTrue((logits.grad[0, :, 1, 1] == 0).all())
        self.assertGreater(float(logits.grad[0, :, 0, 1].abs().sum()), 0)

    def test_iou_accounts_for_false_positives_and_misses(self):
        # Rows = truth, columns = prediction; solid TP3 FP2 FN1 -> IoU .5.
        matrix = np.array([[8, 2, 0, 0], [1, 3, 0, 0], [0, 0, 4, 0], [0, 0, 0, 0]])
        metrics = metrics_from_confusion(matrix)
        self.assertEqual(metrics['iou']['solid'], .5)
        self.assertEqual(metrics['iou']['dashed'], 1.)
        self.assertIsNone(metrics['iou']['crosswalk'])
        self.assertEqual(metrics['foreground_miou'], .75)
        self.assertEqual(metrics['precision']['solid'], .6)
        self.assertEqual(metrics['recall']['solid'], .75)
        self.assertAlmostEqual(metrics['pixel_accuracy'], 15/18)

    def test_nonfinite_gradient_still_stops_without_parameter_update(self):
        model = torch.nn.Linear(2, 1)
        before = model.weight.detach().clone()
        model.weight.grad = torch.tensor([[float('inf'), 1.]])
        with tempfile.TemporaryDirectory() as temporary, redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, 'non-finite'):
                checked_gradient_norm(model, Path(temporary), 1, 2, torch.tensor(1.))
            import json
            record = json.loads((Path(temporary)/'gradient_failure.json').read_text())
            self.assertFalse(record['optimizer_step_performed'])
            self.assertEqual(record['gradients']['weight']['nonfinite_values'], 1)
        self.assertTrue(torch.equal(before, model.weight))

    def test_duration_and_loss_components(self):
        self.assertEqual(duration(3661.9), '01:01:01')
        self.assertEqual(duration(-1), '00:00:00')
        loss, parts = segmentation_loss(torch.zeros(1, 4, 2, 2), torch.zeros(1, 2, 2, dtype=torch.long),
                                        torch.ones(4), return_parts=True)
        self.assertAlmostEqual(float(loss), float(parts['ce']+.5*parts['dice']), places=6)

    def test_four_class_head_preserves_every_other_pretrained_tensor(self):
        binary = load_lane_model('small', 'cpu').state_dict()
        typed = create_model('cpu').state_dict()
        for name, tensor in binary.items():
            if not name.startswith('out_ll.'):
                self.assertTrue(torch.equal(tensor, typed[name]), name)
        torch.set_num_threads(2)
        with torch.inference_mode():
            logits = create_model('cpu').eval()(torch.zeros(1, 3, 384, 640))
        self.assertEqual(list(logits.shape), [1, 4, 384, 640])
        self.assertTrue(torch.isfinite(logits).all())

    def test_nano_gradient_step_and_checkpoint_reload(self):
        torch.set_num_threads(2)
        model = create_model('cpu', 'nano').train()
        pretrained = load_lane_model('nano', 'cpu').state_dict()
        for name, value in model.state_dict().items():
            if not name.startswith('out_ll.'):
                self.assertTrue(torch.equal(value, pretrained[name]), name)
        before = {name: value.detach().clone() for name, value in model.out_ll.named_parameters()}
        images = torch.rand(1, 3, 384, 640)
        target = torch.zeros(1, 384, 640, dtype=torch.long)
        target[:, 100:103, 100:500] = 1
        target[:, 180:183, 100:500] = 2
        target[:, 260:263, 100:500] = 3
        target[:, :12] = 255
        optimizer = torch.optim.AdamW(model.parameters(), lr=3e-4)
        logits = model(images)
        self.assertEqual(list(logits.shape), [1, 4, 384, 640])
        loss = segmentation_loss(logits, target, torch.ones(4))
        loss.backward()
        for parameter in model.parameters():
            if parameter.grad is not None:
                self.assertTrue(torch.isfinite(parameter.grad).all())
        optimizer.step()
        self.assertTrue(any(not torch.equal(before[name], value) for name, value in model.out_ll.named_parameters()))
        model.eval()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/'nano.pt'
            torch.save(dict(state_dict=model.state_dict(), classes=['background', 'solid', 'dashed', 'crosswalk'],
                            variant='nano', preprocessing='RGB /255,640x384 letterbox114'), path)
            reloaded, _ = load_checkpoint(path, 'cpu')
            with torch.inference_mode():
                torch.testing.assert_close(model(images), reloaded(images), rtol=0, atol=0)


if __name__ == '__main__':
    unittest.main()
