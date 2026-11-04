"""Training contracts and CPU inference checks; never call train/backward/step."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

import cv2
import numpy as np
import torch

ROOT = Path(__file__).resolve().parents[1]
TEMP_ROOT = ROOT/'maintenance/light_training_20261019/test_temp'
TEMP_ROOT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT/'code'))
from light_rgb import NAMES, CONTRACT, ColorCrops, build_rgb_model, load_rgb_model, metrics, reject_uncertain
from train_light_selected import verify_file


class SelectedLightContracts(unittest.TestCase):
    def test_unknown_counts_as_miss_and_dangerous_confusion_is_visible(self):
        result = metrics([0, 0, 2], [0, 3, 2])
        self.assertEqual(result['per_class']['red']['recall'], .5)
        self.assertAlmostEqual(result['coverage'], 2/3)
        self.assertEqual(metrics([0], [2])['red_predicted_green'], 1)

    def test_probability_and_margin_both_required(self):
        p = torch.tensor([[.8, .1, .1], [.34, .33, .33], [.5, .05, .45]])
        self.assertEqual(reject_uncertain(p, .4, .15).tolist(), [0, 3, 3])

    def test_wrong_checkpoint_contract_is_rejected(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as directory:
            path = Path(directory)/'legacy.pt'
            torch.save({'names': ['red', 'yellow', 'green', 'off'], 'state_dict': {}}, path)
            with self.assertRaisesRegex(ValueError, 'THREE-color'):
                load_rgb_model(path)

    def test_dataset_reads_bgr_as_rgb_and_does_not_recolor_validation(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as directory:
            path = Path(directory)/'red.png'
            image = np.zeros((30, 60, 3), np.uint8)
            image[:] = [0, 0, 255]
            cv2.imwrite(str(path), image)
            row = {'image': str(path), 'box': [0, 0, 60, 30], 'color': 'red'}
            tensor, label = ColorCrops([row])[0]
            self.assertEqual(label, 0)
            self.assertEqual(tuple(tensor.shape), (3, 128, 128))
            self.assertGreater(float(tensor[0, 64, 64]), float(tensor[2, 64, 64]))

    def test_modified_labels_stop_preflight(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as directory:
            folder = Path(directory)/'bdd_train'
            folder.mkdir()
            image, label = folder/'a.jpg', folder/'a.txt'
            image.write_bytes(b'image fixture')
            label.write_bytes(b'7 .5 .5 .1 .1\n')
            row = {'image': str(image), 'label': str(label), 'group': 'bdd_train',
                   'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
                   'label_sha256': hashlib.sha256(label.read_bytes()).hexdigest()}
            verify_file(row)
            label.write_bytes(b'0 .5 .5 .1 .1\n')
            with self.assertRaisesRegex(ValueError, 'Data changed'):
                verify_file(row)

    @unittest.skipUnless((ROOT/'weights/pretrained/mobilenet_v3_small-047dcff4.pth').exists(), 'Local ImageNet weights not in Git')
    def test_official_initialization_three_outputs_and_safe_checkpoint_roundtrip(self):
        torch.set_num_threads(2)
        model = build_rgb_model(ROOT/'weights/pretrained/mobilenet_v3_small-047dcff4.pth').eval()
        with torch.inference_mode():
            result = model(torch.zeros(1, 3, 128, 128))
        self.assertEqual(tuple(result.shape), (1, 3))
        self.assertTrue(all(parameter.grad is None for parameter in model.parameters()))
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as directory:
            path = Path(directory)/'rgb.pt'
            torch.save({'names': NAMES, 'preprocessing': CONTRACT,
                        'state_dict': model.state_dict(), 'selection': {'color_counts': {'bdd': {'red': 3}}}}, path)
            restored, record = load_rgb_model(path)
            with torch.inference_mode():
                self.assertTrue(torch.equal(result, restored(torch.zeros(1, 3, 128, 128))))
            self.assertEqual(record['names'], ['red', 'yellow', 'green'])


if __name__ == '__main__':
    unittest.main()
