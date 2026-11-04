# 代码验证

项目根目录执行 `python run.py test`，或者 `python -m unittest discover -s tests`。

现有测试检查目标跟踪、早期车道辅助状态、UFLD曲线解码/平滑、交通灯补充数据及部分监督损失。车道四类三个测试文件共21项，检查曲线/标签、目录配对、损失/指标、Small/Nano预训练参数和权重、非有限梯度停止、队列顺序与失败处理。队列逻辑测试中的模拟权重不能证明训练效果；实际CPU/GPU短试训另有runs记录。

单独检查本次车道四类代码：

```powershell
conda run --no-capture-output -n adas_yolo python -m unittest discover -s tests -p 'test_lane4*.py' -v
```

它们不是训练程序，也不能替代真实道路标注评估。
