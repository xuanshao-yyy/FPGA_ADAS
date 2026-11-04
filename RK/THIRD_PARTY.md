# 第三方来源与许可

- ByteTrack-cpp：https://github.com/Vertical-Beach/ByteTrack-cpp ，commit d43805d461a714f65da039981bd5f5d21cf5cf59。固定版本完整跟踪模块，v7针对实时变帧间隔和过期记录进行了有限修改；v8沿用同一跟踪源码，见vendor_live.patch；许可证见vendor/ByteTrack/LICENSE（MIT，Vertical Beach与Yifu Zhang）。
- ByteTrack论文及原实现：https://github.com/FoundationVision/ByteTrack 。本包不把C++移植称为官方发布的本项目二进制，也不把旧v6简化跟踪作为此模块。
- Eigen 3.4.0：https://gitlab.com/libeigen/eigen ，commit 3147391d946bb4b6c68edd901f2add6ac1f31f8c。编译时EIGEN_MPL2_ONLY；许可文本见vendor/eigen/COPYING.MPL2、COPYING.BSD、COPYING.README。矩阵计算静态编译进程序，运行不依赖额外Eigen共享库。
- 当前定位权重：用户提供的YOLO11n十类best.pt，本次未重新训练；使用VM现有Rockchip适配版Ultralytics 8.3.9导出两种静态尺寸并做INT8转换，应用保留原八类。源码为VM已有/home/alientek/ultralytics_yolo11；Ultralytics AGPL-3.0和原模型使用条件仍适用，不把用户训练权重视为无条件可再分发。源权重与运行模型SHA见model_contract.json。
- 当前颜色权重：本项目2026-10-19训练的MobileNetV3-Small红／黄／绿三分类best.pt，基于已有torchvision实现与既有训练数据；本次只导出与INT8量化。torchvision采用BSD许可；模型与数据仍受原来源条款约束。颜色网络没有OFF／非灯类别。
- Small4与pad1：沿用本项目已有四类Small训练和第三版转换记录；架构来源/原始许可保留在原车道套件与third_party/twinlitenetplus_reference。
- Rockchip RKNN、OpenCV：使用已有主包private lib与SDK接口，本包不替换板端系统库或驱动。
- v10 CPU光流新增使用SDK内OpenCV video/calib3d及其完整动态依赖（features2d、flann、dnn、protobuf）。本算法只调用LK、RANSAC与几何函数；链接dnn库不代表在CPU运行车道CNN。依赖与SHA256见verification/library_dependencies.json；许可证在licenses/，protobuf见libprotobuf23_COPYRIGHT.txt。

源码、许可文本与固定版本在本包内保留，模型与原运行文件保留既有来源记录。

Live v7 modifies the vendored ByteTrack for variable source time and bounded removed history. See vendor_live.patch.

原版本曾使用YOLOv8矩形模型；本包替换为YOLO11的640×384及640×640 INT8。车道仍为Small4 pad1 INT8，未替换为Nano4。TwinLiteNetPlus许可见licenses/，ByteTrack/Eigen许可在vendor/，运行库保留原来源。当前ZIP仅供用户本地测试，不上传模型、依赖或道路素材到Git。
