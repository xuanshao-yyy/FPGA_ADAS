# 模型交付

| 目录 / 文件 | 用途 |
|---|---|
| `active_contract.json` | 当前 A 方案模型类型、输入和固定 SHA256 |
| `runtime/` | 当前 YOLOv8n INT8 两种输入、Small4 pad1 INT8、驻车入口副本 |
| `training/runs/` | 八类 YOLOv8n、YOLO26、交通灯加强及 Small4 / Nano4 正式训练权重与记录 |
| `training/light_overnight/` | 交通灯检测 / RGB 分类对比训练留档 |
| `lane_exports/` | Small / Nano 的 ONNX 与 RKNN、补边版本 |
| `pretrained/` | 本交付工具使用的部分原始预训练权重及来源记录 |
| `conversion/` | RKNN 转换脚本；保留原 SDK / 校准输入约定 |

当前运行使用 **YOLOv8n＋HSV、Small4 pad1 INT8**。YOLO26、加强交通灯、RGB 分类与 Nano 为对照留档，未被自动切换为默认部署。正式训练 best / last 保留；早期 smoke 试训与原始训练数据继续留在原工作区，不作为默认交付模型。

当前 `runtime/parking_model.rknn` 与 `yolov8n_bdd8_640x384_i8.rknn` 字节相同。实际安装使用 `RK/models` 及 `RK/parking_model.rknn`，本目录用于集中查找与校验。权重文件大小不是运行内存，训练验证指标不是道路闭环准确率。

原工作区 `weights/` 还保留 UFLD / YOLOP 等历史参考权重，大型第三方参考模型不重复放入本版主用模型目录。每个收录文件的真实来源见上一级 `SOURCE_ORIGINS.json`。
