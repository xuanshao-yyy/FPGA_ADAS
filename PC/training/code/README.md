# 代码怎么找

睡前自动入口：`python run.py train-light-overnight`。实现为`train_light_overnight.py`，顺序运行检测/灯色各最多100轮、完整日志、重复任务检查和临时保持唤醒。`--dry-run`只打印计划。

本轮[交通灯实验入口](../docs/LIGHT_SELECTED_TRAINING.md)：`train_light_selected.py`默认只检查，`--stage detector/color/all`明确启动八类检测或三色分类；`light_rgb.py`是独立三色模型；`run_light_rgb.py`用于训练后的本地视频回放。旧`light_color.py`四色模型仍为历史草稿，不与新三色权重混用。

交通灯新方案目前是[数据审阅阶段](../runs/light_color_dataset_review_20261018/README.md)。`light_color.py`与`prepare_light_color.py`为候选草稿，未接入`run.py`，未运行全量准备，未新增或启动训练入口。不要将其当作已验收的训练流程。

建议从项目根目录执行 `python run.py <命令>`。本目录也可以直接执行脚本，例如 `python code/run_lane_pretrained.py --help`。

| 文件 | 用途 | 统一命令 |
|---|---|---|
| prepare_d2city_annotation.py | D²-City完整视频抽帧、原12类关键帧转YOLO参考、交通灯单类手工审核导出；拒绝覆盖标注工程 | `python code/prepare_d2city_annotation.py --help`，见[标注说明](../maintenance/d2city_labeling_20261021/ANNOTATE.md) |
| prepare_multiview_test.py | 官方nuScenes mini原始相机图转无框视频；六路按时间戳配对，保留视角/时差 | `python code/prepare_multiview_test.py`，见[侧后素材说明](../docs/MULTIVIEW_TEST_DATA.md)；已有输出拒绝覆盖 |
| play_clean_screen.py | 单屏四区域/原视频无控件全屏播放；纯电脑端拍屏工具 | `python code/play_clean_screen.py --start-paused --once`，见[说明](../runs/single_screen_simulation_20261009/README.md) |
| prepare_lane4_data.py / lane4_data.py | 原始BDD曲线转换为背景、实线、虚线、斑马线标签，复用原图 | `run.py prepare-lane4`（已完成，无需重复） |
| train_lane4.py | Small/Nano四类微调，FP32，逐轮时间和关键参数 | `run.py train-lane4 --variant nano --stage check` |
| train_lane4_queue.py | 前一项正常完成后自动开始下一模型，失败则停 | `run.py train-lane4-queue --models small nano`（由用户自行启动） |
| run_lane4.py | 按权重版本加载四类Small/Nano并查看视频 | `run.py lane4 --checkpoint 路径 --show` |
| evaluate_lane4.py | 本次正式Small/Nano最佳权重核验、24张BDD样例及七段同步视频；不训练 | `run.py evaluate-lane4`（本次已完成，结果见项目首页） |
| prepare_lane_frames.py | 将视频提为板端统一640×360 RGB帧，可选已有八类检测框遮挡掩码；不训练 | `run.py prepare-lane-frames --source 视频 --output 新目录`，见[实测包](../deployment/rk3568_lane_suite_20261005/README_上板实测.md) |
| train_traffic_light.py | BDD+Bosch+S2TLD交通灯加强，保留八类输出 | `run.py train-light --stage check` |
| traffic_light_partial_loss.py | 缺失标注的类别不参与补充图片分类损失，仅用于训练 | 由train-light调用 |
| download_s2tld.py / prepare_s2tld.py | 下载校验和S2TLD XML转换、来源分组 | 见交通灯训练说明 |
| train_yolo26.py | YOLO26n八类检查/小样本/正式训练 | `run.py train-yolo26 --stage check` |
| test_videos.py | 正式八类检测模型测试原视频 | `run.py detect` |
| compare_detectors.py | 四份检测权重、五种配置在同一帧上的视频对比 | `run.py compare-detectors` |
| track_videos.py | 检测、ByteTrack 跟踪、框平滑对比 | `run.py track` |
| run_lane_pretrained.py | UFLD 预训练车道线对比 | `run.py lane` |
| run_lane_twinlite.py | 新Small/Nano预训练车道候选短片对比 | `run.py lane-twinlite --show` |
| twinlite_lane.py | 官方权重校验、车道单分支及预处理 | 由lane-twinlite调用 |
| run_lane_assist.py | 早期霍夫线/YOLOP 辅助提示实验 | `run.py lane-legacy` |
| audit_lane_training_data.py | 原始车道线标注盘点，不是转换或训练 | `run.py audit-data` |
| prepare_datasets.py | 首次解压/转换目标检测数据，拒绝覆盖已有数据 | `run.py prepare-data` |
| check_project.py | 目录、数据配置、模型完整性检查 | `run.py check` |
| road_tracker.py | 跟踪内部逻辑 | 由入口调用 |
| lane_assist.py | 早期车道线提取与提示逻辑 | 由入口调用 |
| ufld_lane.py | UFLD 加载、解码、平滑和绘制 | 由入口调用 |
| project_paths.py | 项目根目录及公共路径 | 修改路径时先看这里 |

YOLO26n有独立训练入口，其他目标检测训练仍可通过环境中的 `yolo detect train` 命令执行。车道四类使用独立PyTorch训练器，见[车道训练说明](../docs/LANE4_TRAINING.md)，不要用YOLO命令训练。测试代码放在 `../tests/`；它们验证程序逻辑，不能证明真实视频准确率。
