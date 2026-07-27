# RK3588 Ubuntu 移植与兼容策略

## 已消除的机器绑定

- 不使用 `/home/lmxz` 或固定桌面用户名
- 不假定 UID 1000
- 不固定 `HDMI-1`、`HDMI-A-2` 或 `/dev/dri/card0`
- 不依赖 MediaMTX、摄像头设备号和 RKAIQ socket
- 不依赖公网域名、TURN 帐号或远程信令服务
- 桌面推流默认关闭，不与摄像头争用 MPP

## 仍需满足的硬件条件

不同 RK3588 镜像并不保证媒体栈 ABI 完全相同。目标系统至少需要：

- 可用的 DRM/KMS framebuffer
- `libdrm` headers/runtime
- 与内核匹配的 librga
- 与内核 MPP service 匹配的 Rockchip MPP
- FFmpeg `kmsgrab`
- GStreamer `rawvideoparse`、`h264parse` 和 Rockchip `mpph264enc`
- NetworkManager/nmcli
- `/dev/uinput`（需要远程输入时）
- Go 1.22+ 仅在源码构建阶段需要；构建脚本会优先发现安装用户的 `~/.local/toolchains/go*/bin/go`

安装后或更换系统镜像后应先执行：

```bash
sudo ~/Desktop/RemyDesk/scripts/remydesk-doctor.sh --video
```

## 新设备移植顺序

1. 确认 `cat /proc/device-tree/model` 和 `/dev/dri/card*`。
2. 运行 `drm_fb_probe`，确认 connected connector 对应 CRTC 的 `fb` 非 0。
3. 用 `pkg-config --modversion libdrm librga rockchip_mpp` 确认开发环境。
4. 执行 `scripts/build.sh`，不要直接复制来自另一镜像的动态链接二进制。
5. 执行 `scripts/install.sh`。
6. 执行 `scripts/remydesk-doctor.sh --video`，确认输出的后端不是无提示的软件回退。
7. 先验证文件和 Wi-Fi API，再从页面启动桌面推流。
8. 查看 `journalctl -u remydesk-desktop.service -f`，日志会明确打印 capture、conversion 和 encoder。

## 后端移植原则

- `auto` 优先使用稳定的 `ffmpeg-gstreamer-mpp`，在不同厂商的 RK3588 BSP 上只依赖标准 raw NV12 边界。
- `gstreamer-mpp` 适合已验证 `kmssrc`/DMA-BUF 生命周期正确的镜像；显式配置后仍会先做解码稳定性探测。
- `native-mpp` 不进入自动选择，因为 RGA/MPP 的 DMA-BUF 同步和包缓冲 ABI 在旧镜像间差异较大。
- `/run/remydesk/video-backend-v2` 保存本次启动探测结果；删除它可强制重新探测。
- `REMYDESK_DRM_DEVICE=auto` 会按 connected connector 和 active framebuffer 选择 card，不假定 card0。
- `REMYDESK_MPP_USER` 默认是 `remydesk`，目标镜像应确保该用户属于 `video`、`render` 组。
- HDMI 诱骗器常把 4K 放在首选模式；`remydesk-display-mode.service` 会按输出名称自动发现并固定到
  `HDMI_REQUIRED_MODE=1920x1080`，不绑定 `HDMI-1` 或 `HDMI-2`。
- 默认推流和输入坐标均为 1920×1080。低带宽场景可以显式设置
  `REMYDESK_VIDEO_WIDTH=1280`、`REMYDESK_VIDEO_HEIGHT=720`，正常局域网不应默认降到 720p。
- 文件拖入、站内移动和布局持久化只依赖 HTTP/HTML5，不依赖 RK3588 厂商媒体栈，因此在不同 RK3588
  开发板上的行为一致。
- 从网页直接拖文件到 Windows/Linux 桌面优先适配 Chromium、Chrome 和 Edge；Firefox/Safari 对
  `DownloadURL` 支持有限，应保留页面中的“下载”按钮作为兼容路径。

## 后续发布改进

- 将板卡差异整理为 `/etc/remydesk/hardware.json` capability，而不是产品型号 if/else。
- 为不同 RGA/MPP ABI 建立 CI 构建矩阵和设备验收表。
- 使用 polkit action 替代 `sudo nmcli`，进一步收窄权限。
- 将音频采集做成可选插件，按声卡 capability 启用。
- 摄像头作为独立插件恢复，继续由资源仲裁器保证 MPP 独占。
