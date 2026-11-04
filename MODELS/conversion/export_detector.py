"""Run in the existing Rockchip YOLO11 export environment; new files only."""
from pathlib import Path
import json
import onnx
from ultralytics import YOLO

HERE = Path(__file__).resolve().parent
if __name__ == '__main__':
    for h in (384,640):
        path = HERE/f'traffic_yolo11n_640x{h}.onnx'
        if path.exists():
            raise FileExistsError(path)
        # The exporter writes beside its PT input, so isolate each export.
        import shutil
        directory = HERE/f'export_{h}'
        directory.mkdir()
        shutil.copyfile(HERE/'traffic_yolo11n.pt', directory/'traffic.pt')
        model = YOLO(str(directory/'traffic.pt'))
        assert model.model.yaml['nc'] == 10
        exported = Path(str(model.export(format='rknn', imgsz=[h,640], batch=1,
                                        device='cpu', dynamic=False, opset=12)))
        graph = onnx.load(str(exported))
        assert len(graph.graph.output) == 9
        assert [d.dim_value for d in graph.graph.input[0].type.tensor_type.shape.dim] == [1,3,h,640]
        shutil.copyfile(exported,path)
        print('EXPORTED',path,flush=True)
