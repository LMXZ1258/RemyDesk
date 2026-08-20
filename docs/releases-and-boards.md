# Release与Rockchip板卡适配

## Release为什么仍要在目标板编译

SoC型号不能保证不同厂商镜像拥有相同的RGA、MPP、GStreamer和DRM ABI。直接发布一套动态链接二进制，可能在另一块板上出现：

- 找不到 `librga.so` 或 `librockchip_mpp.so`
- 结构体/像素格式/stride约定不一致
- DMA-BUF可导出但跨模块同步损坏
- GStreamer `mpph264enc` 属性不同

所以Release采用混合模式：

1. GitHub Actions交叉编译无CGO依赖的ARM64 Go/WebRTC程序。
2. 安装器携带其余源码。
3. 目标板使用本机头文件和动态库编译C++、DRM、RGA、MPP组件。
4. 安装后运行稳定性探测，决定实际视频后端。

## 板卡支持等级

- `validated`：在指定板卡和镜像完成安装、Web API、DRM捕获、硬解码验证。
- `portable-baseline`：符合目标 Rockchip SoC 的能力假设，但未承诺特定BSP的零拷贝。
- `needs-device-validation`：已有保守默认配置，必须在实机验收。

支持等级记录在 `profiles/*/profile.conf`。

## 新板验收清单

```bash
cat /proc/device-tree/model
uname -a
pkg-config --modversion libdrm librga rockchip_mpp
gst-inspect-1.0 mpph264enc
sudo /opt/remydesk/libexec/remydesk-doctor.sh --video
```

然后验证：

1. 8010页面和文件操作。
2. Wi-Fi连接反馈与热点生命周期。
3. 1920x1080桌面模式。
4. 30分钟持续推流，无花屏、残影或H.264解码错误。
5. 鼠标坐标、键盘和F11应用全屏。
6. 音画同步与网络波动恢复。

## 发布流程

1. 更新 `VERSION` 和 `CHANGELOG.md`。
2. 在至少一块已验证RK3588板执行构建和doctor。
3. 提交并推送代码。
4. 创建相同版本标签，例如 `v0.2.0`。
5. Release工作流自动上传安装器、源码包和SHA256。
6. 在新板先执行安装器 `--verify-only`，再正式安装。
