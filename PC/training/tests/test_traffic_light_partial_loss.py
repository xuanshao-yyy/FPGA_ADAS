"""Verify actual classification gradients, source isolation and augmentation guards."""
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

import torch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'code'))
from traffic_light_partial_loss import MaskedClassificationBCE, PartialLightTrainer, supervision_mask


class PartialLabelTests(unittest.TestCase):
    def test_unlabelled_classes_have_zero_gradient_only_on_partial_images(self):
        logits = torch.zeros((3, 4, 8), requires_grad=True)
        criterion = MaskedClassificationBCE()
        criterion.mask = supervision_mask(['x/bdd_train/a.jpg', 'x/bosch_train/b.png',
                                           'F:\\data\\s2tld_train\\c.jpg'], 'cpu')
        criterion(logits, torch.zeros_like(logits)).sum().backward()
        self.assertTrue(torch.all(logits.grad[0] != 0))
        self.assertTrue(torch.all(logits.grad[1:, :, :7] == 0))
        self.assertTrue(torch.all(logits.grad[1:, :, 7] != 0))

    def test_full_label_loss_is_unchanged(self):
        logits, targets = torch.randn(2, 4, 8), torch.rand(2, 4, 8)
        criterion = MaskedClassificationBCE()
        criterion.mask = supervision_mask(['x/bdd_train/a.jpg', 'x/bdd_val/b.jpg'], 'cpu')
        self.assertTrue(torch.equal(criterion(logits, targets),
            torch.nn.functional.binary_cross_entropy_with_logits(logits, targets, reduction='none')))

    def test_unknown_sources_fail_instead_of_assuming_complete_labels(self):
        with self.assertRaises(ValueError):
            supervision_mask(['x/unknown/a.jpg'], 'cpu')

    def test_image_mixing_is_rejected(self):
        trainer = object.__new__(PartialLightTrainer)
        trainer.args = SimpleNamespace(mosaic=.3, mixup=0., cutmix=0., copy_paste=0.)
        with self.assertRaisesRegex(ValueError, 'mosaic must be zero'):
            trainer.build_dataset('unused')


if __name__ == '__main__':
    unittest.main()
