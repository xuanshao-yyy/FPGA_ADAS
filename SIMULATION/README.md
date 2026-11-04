# 仿真与软件验证

本目录提供可复现的仿真入口；不会连接 RK、烧录 FPGA、下载数据或启动训练。每次运行使用新的 `output/时间戳`，保留工具日志与 JSON 结果。

## FPGA RTL 快速回归

```powershell
python SIMULATION/fpga/run.py --sim-dir D:/modeltech64_2020.4/win64
```

使用交付 `FPGA/project/rtl`，执行：

1. **旧隔帧门控负对照**：只把 `network_port` 替换为旧版固定夹具，确认 PARK 奇数帧被丢弃。它预期触发 testbench 的断言；验证脚本将符合预期的失败认作负对照通过。
2. **当前 PARK30**：奇偶帧均输出，合成画面仅走主端口，DRIVE 四路与 PANORAMA 路由保持。
3. **当前全景网络包**：队列中的两位模式元数据、切换时旧包归属、TX 等待、QV04 / UDP5200，以及以太网 FCS / IPv4 校验与 RGB565 载荷检查。

长测试入口：

```powershell
python SIMULATION/fpga/run.py --sim-dir D:/modeltech64_2020.4/win64 --full
```

额外运行 `tb_panorama_full` 的两种第三路输入模式，覆盖四个异步输入、DDR 等待、CH1 旋转、DRIVE→PARK→PANORAMA→DRIVE、逐像素 scoreboard 与停源情况。该测试耗时明显更长，默认不执行。`static_input_delay.sv` 是数字仿真用的理想输入延迟模型，不代表真实 IO 时序。

## PC 直接运行的 C++ 逻辑检查

```powershell
python SIMULATION/portable/run.py --cxx g++
```

需要 C++17 编译器，执行交付的原始 `test_panorama_layout`、`test_display_job`、`test_parking_guard`。它们检查通道布局、显示任务失败生命周期、检测框几何与驻车阈值 / 保持 / 恢复断言，不运行 RKNN 模型，不等同于摄像头实测。

## RK ARM/QEMU 与虚拟显示

`RK/tests/` 保留全部核心测试；在 Ubuntu RK SDK 虚拟机运行 `bash RK/scripts/build_vm.sh`（脚本内部进入 RK 目录），完成交叉编译、CTest / QEMU。编译依赖环境变量见根 `OPERATIONS.md`。

`RK/scripts/test_auto_ui_vm.sh` 是原 Xvfb / QEMU 工具栏和面板检查入口，依赖原 VM 的虚拟显示依赖与字体路径。运行前按实际 SDK 设置 `TASK_UI_RUNTIME`、`RK_SYSROOT`、`RK_QEMU`，并建立 `RK/verification` 输出目录。原有 47 项 ARM 检查与 36 次工具栏操作记录在根 `verification/rk_startup_guard_20261102`。

## 历史与本次结果

`history/` 保留原 FPGA 像素 / 网络检查记录；`verification/simulation_20261103/`（位于交付根目录）保存本次实际运行结果。不要把夹具像素、负对照或 QEMU 测试计入实板帧率与道路准确率。FPGA 仿真可以检查逻辑与协议，但不测真实电气、传感器图像质量或硬件功耗。
