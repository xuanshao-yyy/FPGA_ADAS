# FPGA 最终配套工程

**烧录：`FPGA_PARK30_20261030_RC1.sbit`**

- SHA256：`d0b37576c401e53ea3f772990c998ae700e2fd3fda35b6b91a7115d9d5a142fc`。
- 工程：`project/QUAD_SFP0_SFP1_RGB565_JUMBO.pds`。
- 工具 / 器件：PDS 2022.2-SP6.4，PGL50H / FBG484 / -6。
- `project/rtl`、`constraints`、`vendor`：自有逻辑、约束与原厂商依赖。
- `reports`：本烧录版本的综合、映射、布局布线及原始时序报告。

本版在原 PANORAMA RC2 基础上去掉 PARK 隔帧门控，保留 SFP0/SFP1、CH1 旋转及 D6 采集恢复；RK 使用 CH0 前、CH2 左、CH1 右、CH3 后。DRV / PARK / PANORAMA 控制与原接线配套。后摄源约 15 FPS，PARK 合成流约 30 FPS不意味着四路摄像头都为 30 独立新帧。

资源用量以布局布线结果为准，LUT 17,443、FF 15,045、DRM 108。详细资源口径见上一级 `RESOURCE_REPORT.md`。厂商 IP 与生成包装器仅随本地交付按原许可保留，不默认公开分发。
