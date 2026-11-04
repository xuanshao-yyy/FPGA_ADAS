# 启动显示修复、声音开关和恢复通行

RK当前目录：`/userdata/test_auto_drive/rk3568_startup_guard_20261102_rc3`。

打开（RK桌面root终端）：
```bash
cd /userdata/test_auto_drive/rk3568_startup_guard_20261102_rc3
export DISPLAY=:0
export XAUTHORITY=/var/run/lightdm/root/:0
bash start_product.sh --scene lanes -- --preview-fps 30 --preview-sink rk
```
完整退出：同目录执行 `bash stop_product.sh`。日常运行不需要编译虚拟机。

- 顶部“声音开启/关闭”可点击切换。控制当前运行的声音，默认开启；已播放的短音可能自然结束。原报警条件和2秒去重保留。
- 底部“恢复通行”对应空格键，只提交原有请求；红灯或危险时仍拒绝并报警，不控制真实执行器。
- 顶部“切换布局”或Tab切换；V前视实时，N下一素材，P暂停继续。
- 启动助手等待本产品首帧，核验PID/归属，动态发现HDMI层，一次修复并读回后退出。无新增逐帧转换、推理或常驻轮询。

已从测得的重启默认图层值验证自动恢复；完整断电冷启动尚未实测。启动结果见 `results/product_runtime/startup_plane_status.json`，20秒未出现本产品首帧会记录失败，检查输入后重启程序。

安装器保留原目录、阈值及板端实际产品功能。源码和验证位于 `maintenance/rk_startup_guard_20261102`；47项ARM测试及原生按钮检查通过。未证明物理音量或CPU严格零增加。
