# 启动、远程查看与维护

## RK 启停

已安装版本：`/userdata/test_auto_drive/rk3568_startup_guard_20261102_rc3`。双击 RK 桌面“驾驶辅助上位机”，或按根 README 的现有目录命令启动；停止时到所属目录执行 `bash stop_product.sh`。不要并行运行两个版本争抢 8080、UDP 和 NPU。

本包独立安装使用 `python3 install_rk.py --install`，默认目标 `/userdata/test_auto_drive/rk3568_final_20261103`。安装器保留原部署；不会自动停旧服务、启动程序、修改网卡或覆盖桌面图标。新副本采用 `bash start_live_product.sh`；其默认输入为 FPGA 实时画面。

声音按钮、恢复通行与布局切换来自同版原生主程序。布局切换只改变显示区域，模型与风险规则不变。上电后图层属性由启动助手一次修复；结果在 `results/product_runtime/startup_plane_status.json`。先核对这一文件以及 `drive.log`，不要用 GPU 软件转换绕过而引入额外 CPU 视频负担。

## PC 远程入口与令牌

打开 `PC/打开远程上位机.url`，或在浏览器输入 `http://192.168.50.2:8080/`。管理 IP 仍需在同一网络可达；手机移动网络不能直接访问这个局域网地址。

在实际运行目录执行：

```bash
cat results/product_runtime/access_token.txt
```

将结果粘贴进网页连接框。令牌属于运行目录，新装副本会使用自己的令牌。此包没有内置令牌。不能直接双击 `PC/web/index.html` 使用完整上位机，因为前端需要 RK 的 API、鉴权和视频流。

网页“行驶辅助”显示状态与远程控制；“驻车监控”显示四路合成 H.264 视频与目标框；360 模式保留在 RK 本机显示。网页不是 RK 桌面远程镜像。

## 日志和导出到 PC 桌面

- 网页展开“运行日志 · 操作与判定原因”，点击“刷新日志”或“导出日志 JSON”。它读取近期操作 / 判定尾部，默认不是全量历史日志。
- 下载时选择 PC 桌面；若浏览器自动下载，文件先在“下载”文件夹。当前产品没有持续同步日志到 PC 桌面的后台服务。
- “最近事件”查看状态变化记录；“导出记录与证据”下载 `rk_evidence.zip`，与完整后台日志是两种导出。

RK 运行状态与后台日志在**实际运行目录**下的 `results/product_runtime/`：

| 文件 | 内容 |
|---|---|
| `drive.log` | 驾驶主程序标准输出与错误 |
| `parking.log` | 驻车接收、编码与识别输出 |
| `display_mode.json` | 模式与交接异常 |
| `startup_plane_status.json` | 启动图层修复与读回结果 |
| `perception.json` | 最近驾驶观测，非完整历史轨迹 |
| `events.jsonl` / `events.sqlite` | 事件流 / 服务事件数据库 |
| `evidence/` | 已保存的图片、编码短片段 |

判定与用户操作 CSV 在每次驾驶任务的结果目录，网页服务会根据当前进程查找。终端可执行 `tail -n 80 results/product_runtime/drive.log` 或 `tail -f results/product_runtime/parking.log`；Ctrl+C 只退出日志跟随。原始日志含运行路径与现场信息，导出前按实际用途检查分享范围。

## 告警声与自动证据

RK 本机声音开关与 PC 网页“开启告警声”相互独立。PC 浏览器需要一次人工点击以允许播放；网页关闭或声音关闭时，PC 不会继续发声。

驾驶风险在状态变化、有新鲜画面且符合保存条件时尝试保存相关通道图片，自动保存设至少 10 秒间隔。驻车红色事件自动落库，并尝试保存最近完整编码片段；该片段不是完整前后录像。片段未就绪、间隔限制或存储不足都可能使事件只有文字记录。网页中“查看证据”只在文件实际存在时出现；“保存当前证据”可手动提出保存请求。

网页既有“不包含录像片段”说明与后端的短片段保存逻辑存在差异，源码此次按原样留档；实际可用证据以事件附件为准。证据先保存在 RK，不会自动存到 PC 桌面。

红灯显示停车不必然触发声音。A/W/D 选择方向，空格 / “恢复通行”提交运动请求；冲突条件、去重与保持逻辑保留。画面上的目标百分比是检测框面积占单通道画面的比例，不是模型置信度；真实置信度在观测数据的 `score` 字段。

## FPGA 打开与烧录

用 PDS 2022.2-SP6.4 打开 `FPGA/project/QUAD_SFP0_SFP1_RGB565_JUMBO.pds`。器件为 PGL50H-FBG484，速度等级 -6。`vendor/` 保留该工程所需厂商 IP，按原授权使用。

烧录使用 `FPGA/FPGA_PARK30_20261030_RC1.sbit`，可先核验 SHA256。保留 SFP0 / SFP1 原接线；CH1 旋转和 D6 采集恢复保留。需要重新烧录时先停止 RK 产品，再按现有下载器连接流程下载并重新启动 RK；本次文件整理未执行烧录。

切换报 FPGA 控制超时，先检查管理网与视频网的区分、线缆、FPGA 是否就绪，以及 `ip -br addr` / `ip route`。视频接口应按收到的实际链路匹配 192.168.10.1 与 192.168.20.1，不能仅凭网口名称推断；之前曾出现配置地址与实际接线反向，导致控制路由错误。不要直接刷新或删除网卡配置。

## 在虚拟机编译 RK

编译在已有 Ubuntu RK SDK 虚拟机完成，日常 RK 启动不依赖虚拟机。将交付 `RK` 目录复制到虚拟机 Linux 文件系统，安装原 SDK 的 OpenCV、X11/Xft、GStreamer、RGA 与交叉编译器开发依赖。

```bash
cd /你的交付路径/RK
export RK_CXX=/usr/bin/aarch64-linux-gnu-g++
export RK_SYSROOT=/home/alientek/rk3568_linux_sdk/debian/binary
export RKNN_INCLUDE_DIR="$PWD/sdk/rknn/include"
export RK_QEMU=/home/alientek/rk3568_lane_suite_20261005/host_tools/usr/bin/qemu-aarch64
bash scripts/build_vm.sh
```

`build-signal/` 保存新构建及测试结果；交付根目录里的已发布二进制不会自动被构建产物覆盖。转到其他 SDK 目录时相应调整环境变量。Windows 解压后 shell 的执行权限可能丢失，安装器会恢复 `.sh` 和四个 ELF 的权限。

## 模型与训练

默认部署模型见 `MODELS/active_contract.json`，不采用目录名中历史 YOLO11 文案判断当前模型。正式训练结果与备选模型见 `MODELS/README.md`，数据准备 / 训练源码见 `PC/training/code`，转换脚本见 `MODELS/conversion`。

原工作区 `F:\yolo_for_qiansai` 保留原数据与可用训练环境，可先执行 `conda run --no-capture-output -n adas_yolo python run.py --help` 查看入口。本交付 `PC/training` 的代码按原相对项目结构留存，独立使用时需要把数据路径及权重参数指向实际文件；`MODELS` 与原 `runs/weights` 的目录不同，不应无参数启动批量任务。打包不触发训练、转换或下载。

## 回退

先在当前服务所属目录执行 `stop_product.sh`，再回到原目录按原启动方式打开。原始源码、模型、配置、测试素材、历史发布包均未移动或删除。还原 FPGA 时使用与原 RK 版本匹配的历史烧录文件，并按对应历史 README 操作。
