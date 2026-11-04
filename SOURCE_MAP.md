# 代码模块与来源

## RK

| 目录 / 文件 | 职责 |
|---|---|
| `RK/src/main.cpp`、`ai_worker.cpp` | 四路接收、检测推理、任务调度 |
| `RK/src/lane_runtime.cpp` | 车道模型与结果处理 |
| `RK/src/park_stream.cpp`、`parking_runtime.cpp` | 驻车目标检测、编码输出 |
| `RK/src/panorama_view.cpp`、`mode_hub.cpp` | 原生全景显示与模式交接 |
| `RK/include/` | UDP 协议、跟踪、车道规则、显示、信号与证据接口 |
| `RK/product/server.py` | HTTP API、鉴权、模式管理、证据与日志接口 |
| `RK/product/lifecycle.py` | 单实例启动与所属进程退出 |
| `RK/product/startup_planes.py` | 启动时一次修复硬件图层 |
| `RK/product/web/` | 实际由 RK 服务提供的 PC 浏览器界面 |
| `RK/vendor/`、`lib/`、`sdk/` | 原有跟踪依赖、私有运行库与 RKNN 编译头文件 |
| `RK/tests/`、`cmake/`、`CMakeLists.txt` | 核心逻辑测试与 ARM64 构建 |

主用源目录是原工作区 `deployment/rk3568_startup_guard_20261102`，对应 Git `1d96044` 的交付链。主程序 SHA256 固定为 `817b33b7…98c02d7`。`lib/vendor/sdk/models/initial_state` 采用独立交付 `CURRENT_SYSTEM_20261029_RC5` 的实体副本。主程序、src、include 与测试源文件逐一校验，算法不变。

`RK/model_contract_history.json` 与 `README_SOURCE_HISTORY.md` 记录历史对比说明，不作为当前模型 / 入口选择。`install_historical_overlay.py` 是历史 RC2→RC3 增量安装器，保留追溯，新的完整交付请用根目录 `install_rk.py`。

## PC 与模型

`PC/web` 与当前 RK 网页文件逐字节相同，为阅读和修改前端提供独立位置；运行服务使用 `RK/product/web`。它依赖 RK API，不能作为 `file://` 静态文件完整运行。`PC/training` 收录根 `code/`、`tests/`、`run.py` 和所需网络定义，原训练环境和数据保留在工作区。

`MODELS` 区分当前运行权重、正式训练权重与备选模型。YOLO26、加强交通灯和 RGB 分类对比权重不是默认部署模型；是否部署由启动参数和实际文件 SHA256 确定。转换工具保留原环境路径和模型约定，异机复用须按实际 SDK 设置。

## FPGA

工程来自 `releases/FPGA_PARK30_20261030_RC1/project`，烧录文件与 `reports` 来自同目录。RTL、约束、PDS 文件及依赖一并收录；删除的仅为交付副本中的临时日志 / 锁文件，综合和布局布线报告单独保留在 `FPGA/reports`。工程源注释可能含后续文档整理，烧录制品与 10 月 30 日报告分别按哈希登记，不声称本次重新综合。

## 检查范围

打包会验证每个文件 SHA256、核对 RK 核心与已交付 ELF / FPGA sbit 的固定哈希，并解析自有 Python 文件。原始软件测试、板端功能检查与性能测量按日期保留，不从记录中的 PID 推断当前运行状态。本次没有连接成功 RK，不修改运行状态、网卡或 FPGA。

完整逐文件原始路径与来源 SHA256 见 `SOURCE_ORIGINS.json`；交付最终字节见 `SHA256.json`。原始文件来源哈希与规范化 shell 换行后的交付哈希可能不同，适配范围已登记。
