"""Project locations, independent of the shell's current directory."""
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DATA_ARCHIVES = PROJECT_ROOT / 'data_archives'
DETECTOR_WEIGHTS = PROJECT_ROOT / 'runs/bdd8_full_n640/weights/best.pt'
UFLD_WEIGHTS = PROJECT_ROOT / 'weights/ufld_v2/culane_res18.pth'
YOLOP_WEIGHTS = PROJECT_ROOT / 'weights/yolop/yolop-640-640.onnx'
