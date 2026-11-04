# PC 上位机与离线工具

| 位置 | 用途 |
|---|---|
| `打开远程上位机.url` | Windows 双击访问 RK 管理网页，不内置访问令牌 |
| `web/` | 当前浏览器界面的 HTML / CSS / JavaScript 阅读副本 |
| `training/run.py`、`training/code/` | PC 数据整理、训练、评估、检测和跟踪工具 |
| `training/tests/`、`training/third_party/` | 对应测试与网络定义 / 许可 |

实际网页由 `RK/product/server.py` 提供，前端运行路径为 `RK/product/web`。修改阅读副本不会自动改变 RK 页面。网页需要 HTTP API 与令牌，不能直接打开静态 HTML 替代服务。

网络互通后访问 `http://192.168.50.2:8080/`，根据实际运行目录读取令牌。日志导出、最近事件、保存当前证据、导出记录与证据位于网页下方；驻车报警声需要点击“开启告警声”。详见上一级 `OPERATIONS.md`。

训练工具依赖原数据及 Python / PyTorch / Ultralytics 等环境，模型在旁边 `MODELS/`，不复制训练数据。源码仍保留原相对项目结构和部分原环境路径，独立运行前用 `--help` 核对参数；日常远程查看无需这些训练依赖。
