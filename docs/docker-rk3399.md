# AIO-3399J Docker 部署

镜像面向 Firefly AIO-3399J、RK3399、ARM64 和 Firefly Ubuntu 20.04 BSP。它包含 RemyDesk 控制平面、WebRTC 发布器、DRM/RGA/MPP 推流程序，以及固定版本的 Firefly MPP 和 Rockchip RGA 用户态库。

## 前提

- 主机架构必须是 `aarch64`。
- 主机内核、DRM、RGA 和 VPU 驱动必须来自兼容的 RK3399 BSP。
- Docker 使用 `host` 网络，直接监听 TCP 8010 和 8088。
- 为访问 DRM、uinput 和可选的 HDMI connector 强制连接功能，RK3399 Compose 配置使用 `privileged: true`。
- systemd 安装版与 Docker 版不能同时运行。

## 直接运行发布镜像

```bash
git clone https://github.com/LMXZ1258/RemyDesk.git
cd RemyDesk

sudo systemctl disable --now remydesk.service remydesk-desktop.service 2>/dev/null || true
mkdir -p remydesk-data remydesk-state
sudo docker compose -f compose.rk3399.yml up -d
```

旧版 Docker Compose 使用：

```bash
sudo docker-compose -f compose.rk3399.yml up -d
```

浏览器访问：

```text
http://<板卡IP>:8010/
```

查看状态和日志：

```bash
sudo docker ps --filter name=remydesk
sudo docker logs -f remydesk
sudo docker exec remydesk tail -f /var/lib/remydesk/desktop.log
```

停止并恢复 systemd 安装版：

```bash
sudo docker compose -f compose.rk3399.yml down
sudo systemctl enable --now remydesk.service remydesk-desktop.service
```

## 配置

编辑 `compose.rk3399.yml` 的 `environment`。建议至少设置：

```yaml
REMYDESK_AUTH_PASSWORD: "请设置局域网管理密码"
```

默认视频参数是 1920×1080、30 FPS、初始 6 Mbps，并根据 RTCP 在 2–8 Mbps 之间自适应。数据和状态分别保存在当前目录的 `remydesk-data/`、`remydesk-state/`。

主机使用 LightDM 时，默认 Xauthority 是 `/var/run/lightdm/root/:0`。GDM 主机应改为实际路径，例如 `/run/user/1000/gdm/Xauthority`，并确保对应目录已挂载。

## 本地构建

在 ARM64 板卡上：

```bash
sudo docker build -t remydesk:rk3399-local .
```

国内网络若无法访问默认 Go 模块代理，可使用：

```bash
sudo docker build --build-arg GOPROXY=https://goproxy.cn,direct \
  -t remydesk:rk3399-local .
```

在支持 Buildx/QEMU 的电脑上：

```bash
docker buildx build --platform linux/arm64 -t remydesk:rk3399-local --load .
```

构建会下载校验和固定的 Firefly MPP 1.5.0 软件包和 Rockchip librga 1.10.0。镜像不包含内核、DTB 或固件。

## 发布

推送 `main` 或 `v*` 标签后，GitHub Actions 发布：

```text
ghcr.io/lmxz1258/remydesk:rk3399-latest
```

如果仓库配置了 `DOCKERHUB_USERNAME` 和 `DOCKERHUB_TOKEN` secrets，同一工作流也会发布到 Docker Hub 的 `<用户名>/remydesk`。

首次发布后请在 GitHub 的 `Packages → remydesk → Package settings` 检查镜像是否继承公开仓库权限；若仍为 Private，改为 Public，之后板卡才能匿名拉取。镜像已包含 `org.opencontainers.image.source` 标签以关联本仓库。

## 容器限制

- 主界面的“关机”只停止容器，不会关闭宿主机。
- Wi-Fi 管理通过挂载的宿主机 D-Bus 调用 NetworkManager；不同 BSP 的 Polkit 策略可能拒绝修改操作。
- 镜像只解决用户态部署，无法替代设备树、内核 DRM/RGA/VPU 驱动或 HDMI 模式配置。
- `privileged: true` 只适合可信局域网和可信镜像，不建议把 8010/8088 暴露到公网。
