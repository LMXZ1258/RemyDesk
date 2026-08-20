# 第三方组件

- cpp-httplib：MIT License，见 `third_party/httplib.h` 文件头。
- nlohmann/json：MIT License，见 `third_party/json.hpp` 文件头。
- Pion WebRTC 及 Go 依赖：各自许可证由 Go module 元数据声明；Docker 镜像内副本位于 `/usr/share/licenses/remydesk/go-modules/`。
- epub.js：BSD-2-Clause，见 `web/EPUBJS-LICENSE.txt`。
- JSZip：MIT/GPLv3 双许可证，见 `web/JSZIP-LICENSE.markdown`；本项目按 MIT 使用。
- Lucide：ISC License（部分 Feather 衍生图标为 MIT），见 `web/LUCIDE-LICENSE.txt`。

源码 Release 不包含 Rockchip MPP、RGA、GStreamer Rockchip 插件和厂商 BSP，由目标板系统或厂商软件源提供。

RK3399 Docker 镜像例外：镜像包含 Firefly `librockchip-mpp1` 1.5.0 和 Rockchip librga 1.10.0 ARM64 运行库。两者使用 Apache-2.0 许可证；MPP 的 Debian 打包文件包含 GPL-2+ 条款。镜像中的完整说明位于 `/usr/share/doc/librockchip-mpp1/copyright` 和 `/usr/share/licenses/remydesk/librga-COPYING`。镜像仍不包含内核、DTB、固件或内核驱动。
