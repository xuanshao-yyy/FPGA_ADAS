# RK 代码与运行文件

主程序、服务、配置、依赖和主用模型均在本目录，复制后不以软链接依赖旧部署目录。

- 新安装统一入口：`bash start_live_product.sh`。默认 FPGA 实时、硬件显示、驻车 30 FPS / 5 Mbps。
- 完整停止：`bash stop_product.sh`，不是关闭 RK 电源。
- 源码：`src/`、`include/`、`product/`；测试：`tests/`。
- 主用模型：`models/` 与 `parking_model.rknn`；约定：`model_contract.json`。
- 原素材播放启动链保留于原脚本；测试视频不在本包，需自行导入。
- `README_SOURCE_HISTORY.md` / `model_contract_history.json` / `install_historical_overlay.py` 属于追溯资料；本交付的安装入口是上一级 `install_rk.py`。

部署、编译、令牌与日志说明见上一级 `OPERATIONS.md`。本目录是 ARM64 RK 程序，不能在 Windows 直接运行 ELF。
