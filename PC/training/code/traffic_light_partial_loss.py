"""Training-only supervision masks; exported inference network remains standard YOLOv8."""
import torch
from torch import nn
from ultralytics.models.yolo.detect import DetectionTrainer
from ultralytics.utils.loss import v8DetectionLoss
from ultralytics.utils.torch_utils import unwrap_model


def supervision_mask(files, device):
    result = []
    for file in files:
        group = str(file).replace('\\', '/').split('/')[-2]
        if group in ('bdd_train', 'bdd_val'):
            result.append([True] * 8)
        elif group in ('bosch_train', 'bosch_val', 's2tld_train', 's2tld_val', 's2tld_test'):
            result.append([False] * 7 + [True])
        else:
            raise ValueError(f'Unknown supervision source: {file}')
    return torch.tensor(result, device=device, dtype=torch.bool).unsqueeze(1)


class MaskedClassificationBCE(nn.Module):
    def __init__(self):
        super().__init__()
        self.mask = None

    def forward(self, logits, targets):
        if self.mask is None or logits.shape[-1] != 8 or self.mask.shape != (logits.shape[0], 1, 8):
            raise ValueError('Missing or incompatible eight-class supervision mask')
        return nn.functional.binary_cross_entropy_with_logits(logits, targets, reduction='none') * self.mask


class PartialLightLoss(v8DetectionLoss):
    def __init__(self, model):
        super().__init__(model)
        if self.nc != 8 or self.reg_max != 16:
            raise ValueError('This loss was verified only for the current BDD8 YOLOv8 detector')
        self.bce = MaskedClassificationBCE()

    def get_assigned_targets_and_loss(self, preds, batch):
        self.bce.mask = supervision_mask(batch['im_file'], self.device)
        return super().get_assigned_targets_and_loss(preds, batch)


class PartialLightTrainer(DetectionTrainer):
    def build_dataset(self, img_path, mode='train', batch=None):
        # Image mixing would make im_file an incorrect description of label completeness.
        for name in ('mosaic', 'mixup', 'cutmix', 'copy_paste'):
            if getattr(self.args, name) != 0:
                raise ValueError(f'{name} must be zero for partial-label supervision')
        return super().build_dataset(img_path, mode, batch)

    def preprocess_batch(self, batch):
        batch = super().preprocess_batch(batch)
        model = unwrap_model(self.model)
        if not isinstance(getattr(model, 'criterion', None), PartialLightLoss):
            model.criterion = PartialLightLoss(model)
        return batch
