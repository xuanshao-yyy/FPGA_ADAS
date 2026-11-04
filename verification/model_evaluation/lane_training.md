# 四类车道正式训练评估：2026-10-05

**当前建议优先使用Small，保留Nano作后续板端资源对照。Small已能识别主要道路标线，但仍有漏线、虚实线混淆和多余线段，还不能直接据此判断可否变道。**

本次评估使用两份正式训练的`best.pt`，不是试训权重，也不是第30轮的`val_latest.jpg`。两模型均完成30轮、最佳均为第26轮；训练66,640张，验证9,526张，数据划分一致。

## 验证结果

| 指标 | Small | Nano |
| --- | ---: | ---: |
| 前景mIoU：实线、虚线、斑马线的平均值 | **44.63%** | 34.48% |
| 实线IoU | **49.95%** | 42.79% |
| 虚线IoU | **40.92%** | 29.20% |
| 斑马线IoU | **43.03%** | 31.45% |
| 实线像素精确率 / 召回率 | 58.32% / 77.67% | 51.69% / 71.32% |
| 虚线像素精确率 / 召回率 | 49.98% / 69.28% | 38.48% / 54.75% |
| 斑马线像素精确率 / 召回率 | 49.62% / 76.40% | 37.07% / 67.48% |
| 参数量 | 117,976 | 31,323 |
| 30轮耗时合计，含每轮验证和保存 | 约6小时30分钟 | 约5小时37分钟 |

Small前景mIoU高出**10.15个百分点**。Nano参数更少，但不能据此断言实际RK3568四路系统更快：本次没有板测。耗时合计不含训练开始前的初始验证。

这些是BDD验证集**3像素标线中心带上的像素指标**，不是YOLO框mAP，也不是“44.63%的车道识别正确”。中心带轻微错位会明显降低IoU；源标注可能沿整条虚线中心曲线连续连接，不能从蓝色连续带推断物理标线是实线。斑马线也是中心带，不是完整漆面区域。

Small最佳训练/验证loss约0.3999 / 0.4025，Nano约0.5267 / 0.5247；两者后期指标提升趋缓，第30轮前景mIoU分别44.47% / 34.19%。曲线暂未提供继续单纯加轮数能显著改善的依据，也不能据此断言已经达到最佳可实现精度。

- [训练曲线：损失、前景mIoU、分类IoU、每轮耗时](training_curves.png)
- [Small最佳权重](https://github.com/xuanshao-yyy/yolo_for_qiansai/releases/download/artifacts-20261009/5fa3ef21bd_best.pt) / 逐轮数据（仅本地文件：`runs/lane4_small_bdd-5/results.csv`）
- Nano最佳权重（仅本地文件：`runs/lane4_nano_bdd/weights/best.pt`） / 逐轮数据（仅本地文件：`runs/lane4_nano_bdd/results.csv`）
- 核验记录、权重SHA256和视频参数（仅本地文件：`runs/lane4_evaluation_20261005/report.json`）

## 实际道路短片：先看这里

每帧从左到右：**原视频 → Small → Nano**。绿色为实线，蓝色为虚线，紫红为斑马线。这里显示原始四类语义候选，没有道路区域过滤、本车左右边界选择、曲线拟合或时序平滑。

| 原视频 | 本次片段 | 同步对比 | 三帧速览 |
| --- | --- | --- | --- |
| 113539 | 35—47秒，城市双黄线/路口 | 播放（仅本地文件：`runs/lane4_evaluation_20261005/video_20260929_113539_comparison.mp4`） | [图片](113539_overview.jpg) |
| 112507 | 0—12秒，多车道直路 | 播放（仅本地文件：`runs/lane4_evaluation_20261005/video_20260929_112507_comparison.mp4`） | [图片](112507_overview.jpg) |
| 112454 | 约0—3.53秒，源视频不足12秒，斑马线 | 播放（仅本地文件：`runs/lane4_evaluation_20261005/video_20260929_112454_comparison.mp4`） | [图片](112454_overview.jpg) |
| 112739 | 0—12秒，车内立柱/屏幕占比大 | 播放（仅本地文件：`runs/lane4_evaluation_20261005/video_20260929_112739_comparison.mp4`） | [图片](112739_overview.jpg) |
| 113343 | 0—12秒，道路标线/斑马线 | 播放（仅本地文件：`runs/lane4_evaluation_20261005/video_20260929_113343_comparison.mp4`） | [图片](113343_overview.jpg) |
| 113421 | 0—12秒，前车遮挡/斑马线 | 播放（仅本地文件：`runs/lane4_evaluation_20261005/video_20260929_113421_comparison.mp4`） | [图片](113421_overview.jpg) |
| 113452 | 0—12秒，车辆与路边标线 | 播放（仅本地文件：`runs/lane4_evaluation_20261005/video_20260929_113452_comparison.mp4`） | [图片](113452_overview.jpg) |

七段共2,266帧，已逐帧处理并检查保存视频可解码、帧数对应。排除了拍摄车机虚拟辅助驾驶画面的111440视频。这些是已有开发视频，没有人工真值，因此不能给出道路视频准确率，也不视为从未用于开发的保留测试集。

## BDD标注与预测对照

24张可复现的验证样例，每组4张：夜间、黄昏、斑马线、虚线、实线、无前景标线标注。选择记录见`report.json`。每行从左到右：原图、BDD标签、Small、Nano；这些样例不替代全验证集指标。

- [对照1：夜间及部分黄昏](validation_overview_1.jpg)
- [对照2：黄昏及斑马线](validation_overview_2.jpg)
- [对照3：虚线及部分实线](validation_overview_3.jpg)
- [对照4：实线及无前景标注样例](validation_overview_4.jpg)

夜间湿路、阴影和遮挡下仍有漏线；Nano较容易出现碎片和类别混淆。无前景标注图中的预测需结合原图检查，源标注完整性未知，不能把每个额外像素都当作已确认误检。

## 代码与复现

在项目根目录PowerShell查看Small短片，`--show`弹窗，Q退出：

```powershell
conda run --no-capture-output -n adas_yolo python run.py lane4 --checkpoint runs/lane4_small_bdd-5/weights/best.pt --show
```

重新生成本次对比，固定使用上面两份正式结果；只推理，不训练，新建结果目录：

```powershell
conda run --no-capture-output -n adas_yolo python run.py evaluate-lane4
```

代码在[评估脚本](../../code/evaluate_lane4.py)。它严格重载最佳权重，核对数据配置/索引SHA256和最佳轮次；全验证指标从最佳权重保存的混淆矩阵重新计算，并与逐轮CSV核对，**没有重新执行9,526张全量验证**。本次额外运行的是24张样例和七段视频的推理。原模型权重、原视频及数据标签保持原位。
