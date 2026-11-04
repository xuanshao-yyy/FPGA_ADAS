# 芯星智能交通 · 最终交付版

**版本：FINAL-20261103｜平台：FPGA PGL50H＋RK3568＋PC 浏览器**

四路视频接入、目标检测与跟踪、车道和信号提示、侧后风险提醒、驻车守护、远程操作与事件复核的统一代码及制品交付。以 2026 年 11 月 2 日 RK 显示修复版本与 PARK30 FPGA 为固定组合；本次整理不修改模型、推理调度或告警判断。

## 一眼找到入口

| 文件夹 / 文件 | 内容 | 常用入口 |
|---|---|---|
| `RK/` | C++ 主程序、Python 服务、网页、动态库、部署模型、配置及构建依赖 | `start_live_product.sh` / `stop_product.sh` |
| `MODELS/` | 主用 RKNN、正式训练权重、车道导出、模型转换代码及模型约定 | `active_contract.json` |
| `FPGA/` | 当前 PDS 工程、RTL、约束、厂商 IP、本版本烧录文件及工具报告 | `FPGA_PARK30_20261030_RC1.sbit` |
| `PC/` | 浏览器上位机源码、远程网址入口、训练与数据处理源码 | `打开远程上位机.url` |
| `SIMULATION/` | FPGA testbench、负对照、ModelSim 与 PC C++ 回归入口、ARM/QEMU 说明 | `SIMULATION/README.md` |
| `verification/` | 按日期保存的测量、软件检查和板端功能记录 | 与资源报告中的来源逐项对应 |
| `RESOURCE_REPORT.md` | 视频、识别频率、CPU、FPGA 用量及测量条件 | 性能与资源表 |
| `OPERATIONS.md` | 启停、令牌、日志、告警、证据导出、编译和常见问题 | 操作说明 |
| `SOURCE_MAP.md` / `SOURCE_ORIGINS.json` | 模块关系、逐文件原始位置与哈希 | 追溯代码来源 |
| `verify.py` / `SHA256.json` | 完整性核验 | `python verify.py` |

当前 PC 上位机采用浏览器访问 RK 服务。`PC/web` 是便于阅读的同版前端副本，实际服务使用 `RK/product/web`；PC 训练源码属于离线工具，日常查看视频不需要启动训练环境。

## 固定硬件与模型组合

| 项目 | 最终交付选择 |
|---|---|
| RK 主程序 | `rk_signal_control`，SHA256 `817b33b72f8992d8017ac47449620cdd2ca750aab79a3c8a53b86399798c02d7` |
| FPGA 烧录文件 | **`FPGA/FPGA_PARK30_20261030_RC1.sbit`** |
| FPGA 文件 SHA256 | `d0b37576c401e53ea3f772990c998ae700e2fd3fda35b6b91a7115d9d5a142fc` |
| 主用检测 / 灯色 | YOLOv8n INT8＋HSV，A 方案 |
| 检测输入 | 前视 640×384；侧后合成输入 640×640 |
| 车道 / 跟踪 | Small4 pad1 INT8 / ByteTrack |
| 通道角色 | **CH0 前、CH2 左、CH1 右、CH3 后** |
| 驻车视频 | 1280×720 H.264，目标 30 FPS / 5 Mbps |
| 网页 | `http://192.168.50.2:8080/`，需管理网络互通与访问令牌 |

烧录文件保留原文件名以对应工具报告和校验值。旧 PANORAMA RC2 的驻车输出存在隔帧门控，不能替代本组合中的 PARK30 文件。

## 已有测量摘要

本次新增检查（2026-11-03）：**3 项 FPGA 快速仿真回归及 3 项 PC C++ 逻辑测试通过**。其中一项 FPGA 检查是旧隔帧门控的预期失败负对照；完整异步源 / DDR 逐像素长仿真入口一并提供。结果见 `verification/simulation_20261103/`。

| 测量项目 | 结果 | 数据条件 |
|---|---:|---|
| FPGA 驻车合成画面传输 | **30.07 FPS** | 2026-10-30，约 12 秒实链路采样 |
| RK H.264 编码输出 | **29.88 FPS** | 同次采样，剔除前 2 秒预热 |
| PC 浏览器呈现 | **30.07 FPS** | 同日单台 Chrome，约 12 秒，记录丢帧 0 |
| RK 驻车整机 CPU | **31.12%** | 同次采样，`/proc/stat` 全核总量口径 |
| 四路驻车目标检测 | **9.24 / 9.28 / 9.25 / 9.81 Hz** | CH0 / CH1 / CH2 / CH3，样本状态读数 |
| FPGA LUT | **17,443 / 42,800，40.75%** | 本版本布局布线报告 |
| FPGA FF | **15,045 / 64,200，23.43%** | 同上 |
| FPGA DRM | **108 / 134，80.60%** | 同上 |

视频显示帧率、模型识别频率与摄像头独立新帧率是三个指标。后摄源约 15 FPS，合成流达到 30 FPS 并不代表后摄产生 30 个独立新帧。上述数值来自随包保留的历史测量，不是 11 月 3 日重新测得的整机数据；详细口径见资源报告。

## 已安装的 RK 怎么打开

现有 RK 运行目录仍为：

```text
/userdata/test_auto_drive/rk3568_startup_guard_20261102_rc3
```

RK 桌面的“驾驶辅助上位机”使用这个目录。原程序、模型、素材、配置和历史部署目录均保留，本交付整理不会替换正在运行的程序。

```bash
# 在现有 RK 终端运行
cd /userdata/test_auto_drive/rk3568_startup_guard_20261102_rc3
DISPLAY=:0 XAUTHORITY=/var/run/lightdm/root/:0 PREVIEW_BACKEND=rk \
  bash start_product.sh --scene lanes -- --preview-fps 30 --preview-sink rk

# 完整退出程序，RK 本身不会关机
bash stop_product.sh
```

新机器或需要独立安装时，把完整交付目录传到 RK，在交付目录执行：

```bash
python3 verify.py
python3 install_rk.py                 # 仅检查路径、文件和空间
python3 install_rk.py --install       # 安装到新的 rk3568_final_20261103 目录
# 到旧服务所属目录执行 stop_product.sh 后，再启动新副本：
cd /userdata/test_auto_drive/rk3568_final_20261103
DISPLAY=:0 XAUTHORITY=/var/run/lightdm/root/:0 bash start_live_product.sh
```

新副本默认四路 FPGA 实时输入；原版素材播放入口也保留。测试视频、数据集和历史运行日志不重复打包，使用本地素材时从网页素材库导入。未导入素材时“下一素材”没有可切换内容。

## 界面操作

- 顶栏“切换布局”或 **Tab**：原小车四路布局与前视主导布局往返切换。
- **A / W / D**：路口左转 / 直行 / 右转意图；**J / M / L**：左变道 / 右变道 / 取消变道。
- **空格 / 恢复通行**：提交恢复请求，仍受红灯与风险规则约束；**C** 重新核查。
- **P / N / V**：暂停画面 / 下一素材 / 前视实时；暂停画面不等于停止系统判断。
- 顶栏声音按钮控制 RK 本机提示音；驻车网页“开启告警声”控制 PC 浏览器提示音。
- 页面下方“运行日志”查看近期判定，“最近事件”查看记录，“导出记录与证据”保存证据包。

路口方向按钮只选择意图，不直接表示车辆已经行驶。普通红灯等待可以仅显示停车；红灯条件下请求恢复通行会进入冲突告警。此产品提供辅助提示与模拟状态，不连接真实车辆执行器。

## 文件完整性与分发

先运行 `python verify.py`，核对随包文件大小及 SHA256。`SOURCE_ORIGINS.json` 记录来源；C++ 核心、判断规则与模型均采用原文件副本。唯一入口适配是复用既有实时启动器、分离历史模型说明及规范化 shell 换行。

完整本地 ZIP 包含模型、二进制和厂商依赖，仅按各原许可使用。Git 保存自有源码、打包脚本、说明和检查结果；模型、动态库、sbit、厂商 IP 和 ZIP 按仓库制品规则保留本地，并以 SHA256 登记。访问令牌、SSH 私钥与用户历史日志不进入交付包。
