# DRM/KMS -> RGA -> MPP -> RTSP prototype

This is an experimental Rockchip desktop capture path for `lmxz-desktop`.

RemyDesk does not select this native encoder automatically. Vendor RGA/MPP
buffer synchronization differs across BSP images, so use the portable selector
for production and require an FFmpeg decode check before enabling this backend.

Implemented stages:

1. `drm_fb_probe`
   - Enumerates DRM connectors, CRTCs, planes and framebuffers.
   - Verifies whether the active HDMI framebuffer can be exported as DMA-BUF.

2. `drm_rga_snapshot`
   - Exports the active HDMI framebuffer as DMA-BUF.
   - Uses RGA to convert `XR24` to `NV12`.
   - Writes one raw NV12 frame to `/tmp/drm-rga-frame.nv12`.

3. `drm_rga_mpp_frame`
   - Exports the active HDMI framebuffer as DMA-BUF.
   - Allocates a DRM-backed `MppBuffer`.
   - Uses RGA to convert the HDMI framebuffer into that MPP buffer.
   - Uses MPP to encode one H.264 frame.

4. `drm_rga_mpp_stream`
   - Keeps the DRM device, RGA imports, MPP encoder and MPP DRM buffer alive.
   - Captures the current HDMI KMS framebuffer continuously.
   - Uses RGA to convert the scanout framebuffer into the MPP buffer.
   - Can use RGA to alpha-blend the DRM cursor plane into the image.
   - Writes Annex-B H.264 to stdout.

Current cursor mode:

- Xorg is configured with `/etc/X11/xorg.conf.d/20-software-cursor.conf`.
- `Option "SWcursor" "true"` makes Xorg draw the pointer into the primary framebuffer.
- `kms.service` uses `CURSOR=0`, so the KMS capture path does not do RGA cursor composition.

5. `push_rtsp_stream.sh`
   - Runs `drm_rga_mpp_stream`.
   - Uses `ffmpeg -c copy` only as an RTSP muxer/publisher.
   - Does not use GStreamer and does not use ffmpeg for encoding.

6. `push_rtsp_demo.sh`
   - Loops `drm_rga_mpp_frame`.
   - Uses `ffmpeg -c copy` only as an RTSP muxer/publisher.
   - Kept only as a simple debug fallback.

Build:

```bash
cd ~/drm-kms-mpp-rtsp
make
```

Probe HDMI framebuffer export:

```bash
sudo ./drm_fb_probe
```

Capture one NV12 frame:

```bash
sudo ./drm_rga_snapshot /dev/dri/card0 /tmp/drm-rga-frame.nv12
```

Capture and encode one H.264 frame:

```bash
sudo ./drm_rga_mpp_frame /dev/dri/card0 /tmp/drm-rga-mpp-frame.h264
ffprobe -f h264 /tmp/drm-rga-mpp-frame.h264
```

Generate a short raw H.264 stream:

```bash
sudo ./drm_rga_mpp_stream -c /dev/dri/card0 -f 30 -b 12000000 -n 90 > /tmp/drm-rga-mpp-stream.h264
ffprobe -f h264 /tmp/drm-rga-mpp-stream.h264
```

Publish RTSP/WebRTC stream:

```bash
sudo ./push_rtsp_stream.sh
```

Runtime options:

```bash
# Disable cursor composition if software cursor is enabled in Xorg.
sudo CURSOR=0 ./push_rtsp_stream.sh

# Try alternate color conversion if the browser view still looks wrong.
sudo COLOR=bt601 ./push_rtsp_stream.sh
sudo COLOR=bt709 ./push_rtsp_stream.sh
sudo YUV=nv21 ./push_rtsp_stream.sh
```

Open:

```text
http://<board-ip>:8889/drm/
```

Current limitations:

- Root is required. As normal user, `drmModeGetFB2` hides GEM handles for the active framebuffer.
- The RTSP publisher still uses ffmpeg for muxing/publishing only. The capture, color conversion and H.264 encoding path is DRM/KMS -> RGA -> MPP.
- The hardware cursor is a separate DRM plane. `drm_rga_mpp_stream` can composite it with RGA; the one-frame encoder still only captures the main plane.
- The current service prefers Xorg software cursor and disables RGA cursor composition to reduce stutter.
- If the HDMI mode or pixel format changes, restart the stream process so MPP can be reconfigured.

Optional systemd install:

```bash
sudo cp kms.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl start kms.service
sudo systemctl status kms.service --no-pager
```

Do not enable it at boot until it is stable on your machine:

```bash
sudo systemctl enable kms.service
```
