# RemyDesk

RemyDesk 是面向 Rockchip RK3588/RK3588S、RK3399 Linux 开发板的局域网文件桌面、Wi-Fi 配置和低延迟远程桌面项目。

它由三个主要部分组成：

- `remydeskd`：C++17 控制平面，提供文件、网络、热点、便签、服务管理和 HTTP API。
- `drm_rga_mpp_stream`/可移植视频脚本：DRM/KMS 捕获，按当前 BSP 选择 Rockchip MPP 硬编码链路。
- `remydesk-webrtc-publisher`：Go/Pion WebRTC 服务，负责浏览器视频、音频和输入控制。

## 功能

- 桌面式文件管理、上传下载、目录间拖放和图标位置持久化
- 图片、视频、Markdown 和 EPUB 阅读
- Wi-Fi 扫描、连接、动态/静态 IPv4、开机热点
- DRM/KMS 桌面捕获和 Rockchip MPP H.264 硬编码
- 浏览器 WebRTC 播放、鼠标键盘输入和应用全屏控制
- 1920x1080 默认输出，网络状况较差时可显式降到 1280x720
- systemd 安装、诊断、升级和卸载
- 通用 RK3588、Orange Pi 5 Plus、Firefly RK3588 配置档案
- Firefly AIO-3399J ARM64 Docker/Compose 部署和 2–8 Mbps 自适应码率

RemyDesk 不打包厂商内核、DTB、RGA、MPP或闭源GStreamer插件。目标板必须由其BSP提供匹配组件。

## 从 GitHub Release 安装

推荐先下载并检查安装器：

```bash
curl -fLO https://github.com/LMXZ1258/RemyDesk/releases/latest/download/RemyDesk-RK3588-installer.sh
curl -fLO https://github.com/LMXZ1258/RemyDesk/releases/latest/download/SHA256SUMS
grep '  RemyDesk-RK3588-installer.sh$' SHA256SUMS | sha256sum -c -
sudo bash RemyDesk-RK3588-installer.sh
```

也可以使用引导脚本：

```bash
curl -fsSL https://raw.githubusercontent.com/LMXZ1258/RemyDesk/main/scripts/install-release.sh | sudo bash
```

指定板卡配置：

```bash
sudo bash RemyDesk-RK3588-installer.sh --profile orangepi-5-plus
sudo bash RemyDesk-RK3588-installer.sh --profile firefly-rk3588
sudo bash RemyDesk-RK3588-installer.sh --allow-non-rk3588 --profile firefly-rk3399
```

未知RK3588板卡使用 `generic-rk3588`。安装器会在目标板上编译C++、DRM、RGA和MPP组件，以匹配该板BSP；Release只预置与系统ABI无关的ARM64 Go程序。

## 访问与首次安全设置

安装完成后访问：

```text
http://<板卡IP>:8010/
```

默认热点密码是 `12345678`，仅用于首次配置。正式使用前请修改：

```bash
sudo editor /etc/remydesk/remydesk.env
sudo systemctl restart remydesk.service
```

建议同时设置 `REMYDESK_AUTH_PASSWORD`。RemyDesk默认面向可信局域网；公网访问应放在HTTPS反向代理或VPN后面。

## 源码构建

目标板需要CMake、C++17、Go 1.22+、libdrm、librga、Rockchip MPP、FFmpeg和带 `mpph264enc` 的GStreamer。

```bash
git clone https://github.com/LMXZ1258/RemyDesk.git
cd RemyDesk
./scripts/build.sh
ctest --test-dir build --output-on-failure
sudo ./scripts/install.sh
```

替换占用8010端口的旧LAN Disk：

```bash
sudo ./scripts/install.sh --replace-landisk
```

## AIO-3399J Docker部署

Firefly AIO-3399J可以直接使用ARM64镜像：

```bash
sudo systemctl disable --now remydesk.service remydesk-desktop.service 2>/dev/null || true
sudo docker compose -f compose.rk3399.yml up -d
```

默认镜像：

```text
ghcr.io/lmxz1258/remydesk:rk3399-latest
```

容器需要访问DRM、VPU、RGA、uinput和宿主机显示会话，因此Compose配置使用host网络和privileged模式。详细要求、数据目录、Xauthority及恢复systemd版本的方法见 [AIO-3399J Docker部署](docs/docker-rk3399.md)。

## 诊断与卸载

```bash
sudo /opt/remydesk/libexec/remydesk-doctor.sh
sudo /opt/remydesk/libexec/remydesk-doctor.sh --video
sudo journalctl -u remydesk.service -u remydesk-desktop.service -f
sudo ./scripts/uninstall.sh
```

桌面推流安装后默认关闭，需要从Web页面启动。

## Release维护

`VERSION` 是唯一版本来源。推送与其一致的标签会自动生成Release：

```bash
git tag v0.3.0
git push origin v0.3.0
```

GitHub Actions会执行C++/Go测试、shell语法检查，并生成：

- `RemyDesk-RK3588-installer.sh`：latest稳定文件名
- `RemyDesk-RK3588-<version>-installer.sh`：版本固定安装器
- `RemyDesk-<version>-source.tar.gz`：源码包
- `SHA256SUMS`：完整性校验

本地生成同样资产：

```bash
./scripts/build-release.sh
```

## 跨板卡原则

- 不固定用户名、UID、HDMI输出名或 `/dev/dri/card0`
- 自动发现活动DRM connector和framebuffer
- MPP/RGA按实际BSP编译和探测，不仅检查库是否存在
- 不稳定DMA-BUF链路自动选择兼容硬件路径
- 板卡差异放在 `profiles/`，避免污染核心代码
- 每块新板安装后必须运行 `remydesk-doctor.sh --video`

详见：

- [架构说明](docs/architecture.md)
- [跨板卡移植](docs/portability.md)
- [Release与板卡适配](docs/releases-and-boards.md)
- [贡献指南](CONTRIBUTING.md)
- [安全策略](SECURITY.md)

## 许可证

RemyDesk使用MIT许可证。第三方组件见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
