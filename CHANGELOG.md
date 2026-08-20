# Changelog

本项目遵循 [Semantic Versioning](https://semver.org/)。

## 0.3.0 - 2026-08-20

- 增加 Firefly AIO-3399J/RK3399 配置档案和 ARM64 Docker/Compose 部署。
- DRM→RGA→MPP 原始画面使用零 CPU 拷贝链路，并减少编码码流进程间复制。
- 增加 2–8 Mbps 自适应码率、拥塞反馈、静态桌面动态降帧和浏览器自动重连。
- 增加下载任务队列、旧桌面会话快速回收和主界面管理功能。
- 增加 Main Profile、CABAC、1080p30 默认参数及硬件/BSP 诊断改进。

## 0.2.0 - 2026-07-27

- 整理为标准 GitHub 开源仓库结构。
- 增加 GitHub Actions 测试与 Release 自动构建。
- Release 安装器可在 x86 GitHub Runner 上生成，并内置 ARM64 Go/WebRTC 发布器。
- C++、DRM、RGA、MPP 组件继续在目标 RK3588 板上针对 BSP 编译。
- 增加通用 RK3588、Orange Pi 5 Plus 和 Firefly RK3588 配置档案。
- 增加从 GitHub Release 下载、校验和安装的引导脚本。
- 增加贡献、安全、第三方许可证和跨板卡发布文档。

## 0.1.0

- C++17 控制平面、文件桌面、Wi-Fi/热点管理。
- DRM/KMS 桌面捕获、Rockchip MPP H.264 编码和 Pion WebRTC。
- systemd 安装、诊断和单文件安装器。
