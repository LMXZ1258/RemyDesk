# RemyDesk 架构与源码边界

## 1. C++ 控制面

入口是 `src/core/main.cpp`。程序读取环境配置、创建状态目录、写入硬件探测结果，然后启动 HTTP server。

| 模块 | 文件 | 职责 |
| --- | --- | --- |
| 配置 | `src/core/config.*` | 将环境变量转换为强类型路径、端口和策略 |
| 进程执行 | `src/core/process.*` | `fork/execvp`、stdout/stderr 收集、超时和信号终止 |
| 文件服务 | `src/core/file_service.*` | 存储根目录约束、目录操作、便签 |
| HTTP | `src/core/http_server.*` | 静态资源、JSON API、流式上传下载、Basic Auth |
| 网络 | `src/core/network_manager.*` | 通过结构化参数调用 `nmcli`，处理扫描、连接、热点和 IPv4 |
| 服务 | `src/core/service_manager.*` | 只允许桌面服务开关与系统关机 |
| 探测 | `src/core/hardware_probe.*` | 设备树、DRM connector、RGA/MPP/DRM 运行条件 |

系统命令使用 `std::vector<std::string>` 传给 `execvp`。例如 SSID 和密码是独立 argv，不能改变命令结构，
避免旧版 Python 中常见的 shell 拼接注入问题。

## 2. 文件路径安全

客户端只提交相对路径。`FileService::resolve()` 会：

1. 拒绝 `..`。
2. 将路径拼到配置的 storage root。
3. 使用 `weakly_canonical` 解析符号链接。
4. 比较规范化路径前缀，拒绝逃逸到 storage root 外。

上传由 `cpp-httplib::ContentReader` 分块接收，先写 `.uploading.*` 临时文件，成功后使用 rename 原子替换目标。
下载使用文件 content provider，不会把大文件整体读入内存。

桌面图标布局以归一化坐标保存到 `/var/lib/remydesk/desktop-layout.json`，不依赖浏览器 profile 或
`localStorage`，所以板卡重启、浏览器重启和不同窗口尺寸下都能恢复相对位置。文件拖放移动调用 `/api/move`，
源路径和目标目录都会再次经过 `FileService::resolve()`，不能越过 storage root，也不允许把目录移动到自身内部。

浏览器拖入使用标准 `DataTransfer.files`，可上传到当前目录或鼠标指向的子目录。拖出到电脑使用 Chromium 的
`DownloadURL` 数据类型，同时提供 `text/uri-list` 回退；不支持 `DownloadURL` 的浏览器仍可使用普通下载按钮。

## 3. 桌面视频链路

`native/desktop-streamer/portable_h264_stream.sh` 是运行时后端选择器。它不会只检查元素是否存在，而会把探测码流
交给 FFmpeg 解码；只有码流稳定时才缓存并启用对应后端。

| 后端 | 数据路径 | 自动选择 | 说明 |
| --- | --- | --- | --- |
| `ffmpeg-gstreamer-mpp` | DRM/KMS → FFmpeg kmsgrab/CPU NV12 → GStreamer MPP H.264 | 默认优先 | 当前 Orange Pi BSP 上稳定，编码由 RK3588 VPU 完成 |
| `gstreamer-mpp` | KMS/DMA-BUF → `mpph264enc` | 仅在桥接后端不可用时自动尝试，也可显式选择 | 更接近零拷贝，但部分旧 BSP 会不出帧或产生损坏码流 |
| `ffmpeg-software` | DRM/KMS → CPU 转换 → libx264 | 最终回退 | 兼容性最高，CPU 占用和延迟较高 |
| `native-mpp` | DRM/DMA-BUF → RGA → MPP | 不自动选择 | 实验后端，用于板级调试和新媒体栈适配 |

当前稳定链路：

```text
DRM/KMS framebuffer
        |
        v  FFmpeg kmsgrab + hwdownload/scale
bounded raw NV12 FIFO (one producer, one consumer)
        |
        v  unprivileged gst-rockchip mpph264enc
Rockchip MPP H.264
        |
        v
Annex-B -> Pion RTP -> WebRTC
```

`remydesk-display-mode.service` 在图形登录管理器启动后自动寻找当前 Xorg 的 `-auth` 文件和已连接 HDMI 输出，
把诱骗器常见的 3840×2160 首选模式纠正为 `1920×1080@60Hz`。视频输出、Xorg framebuffer 和 uinput
绝对坐标使用同一尺寸，避免重复缩放和鼠标位置偏移。推流启动前还会短暂复查一次模式，以处理热插拔。

KMS 采集仍以 root 运行，因为旧内核会向普通用户隐藏活动 framebuffer handle；MPP 编码子进程降权到 `remydesk`
用户运行。这样既缩小权限，也规避了当前 Orange Pi 镜像中 `mpph264enc` 以 uid 0 运行时产生损坏包的问题。

`drm_hotplug_stream.sh` 和 `drm_rga_mpp_stream.c` 继续自动遍历 `/dev/dri/card*`，用于显式 `native-mpp` 调试。

## 4. 局域网 WebRTC

`native/webrtc-publisher` 只允许 LAN signaling 和 direct Annex-B command source。

1. 浏览器访问 `:8088`，创建 PeerConnection 和 offer。
2. Pion 在本机 HTTP/WebSocket 端点接收 offer，创建 answer。
3. PeerConnection connected 后启动已探测的视频后端子进程。
4. 发布器解析 Annex-B NAL，按 Access Unit 组帧。
5. 大 NAL 使用 FU-A 分片，写入 Pion H.264 RTP track。
6. Pion 处理 ICE、DTLS、SRTP、RTCP 和 UDP 发送。
7. 浏览器的 PLI/FIR 经 RTCP 触发 `SIGUSR1`，编码器请求新 IDR。
8. 鼠标键盘事件经 DataChannel 到 `/dev/uinput`。

RemyDesk v0.1 没有公网 WebSocket 信令、TURN 凭据或远程 publisher systemd 服务。

## 5. 服务生命周期

`remydesk.service` 启动 C++ 控制面。`remydesk-desktop.service` 不带 `WantedBy`，安装后保持关闭。
页面调用 C++ API 后，控制面通过受限 sudoers 启停桌面单元。关闭页面不会自动停止服务，用户可明确点击停止。

硬件编码器只在 WebRTC peer connected 后启动；没有观看者时，发布服务本身不会持续占用 MPP 编码器。
Annex-B NAL 队列默认限制为 8，网络短暂背压不会累积成数秒旧画面。
