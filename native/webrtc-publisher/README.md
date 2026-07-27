# RemyDesk LAN WebRTC adapter

This Go/Pion component is intentionally limited to the local network. It does
not connect to a public signaling server and does not contain default TURN
credentials.

```text
DRM/RGA/MPP Annex-B stdout
          |
          v
NAL parsing -> Access Unit grouping -> RTP/FU-A -> Pion WebRTC
                                                |
                                                v
                                      LAN browser on :8088
```

Build and test:

```bash
go test ./...
go build -o remydesk-webrtc-publisher .
```

Required runtime settings:

```text
SIGNAL_MODE=lan
SOURCE_MODE=command
H264_WORKDIR=/opt/remydesk/libexec
H264_COMMAND=exec ./portable_h264_stream.sh
REMYDESK_VIDEO_BACKEND=auto
```

`portable_h264_stream.sh` selects a backend at runtime:

- `auto`: run a live MPP stability probe once per boot, cache the result under
  `/run/remydesk`, and fall back to FFmpeg software encoding when the vendor stack is
  missing or produces invalid H.264.
- `gstreamer-mpp`: require `kmssrc`, `mpph264enc` and `h264parse`.
- `ffmpeg-software`: use `kmsgrab` and libx264 as the compatibility backend.
- `native-mpp`: explicitly select the experimental DRM/RGA/MPP helper.

The DRM device, video backend and software fallback dimensions can be
overridden with `REMYDESK_DRM_DEVICE`, `REMYDESK_VIDEO_BACKEND`,
`REMYDESK_SOFTWARE_WIDTH` and `REMYDESK_SOFTWARE_HEIGHT`. PulseAudio server
and monitor names use automatic discovery when `PULSE_SERVER=auto` and
`AUDIO_SOURCE=auto`.

RemyDesk keeps this protocol adapter in Go because Pion supplies the ICE,
DTLS, SRTP, RTP/RTCP and browser interoperability layer. Device control,
storage, network configuration and service orchestration remain in C++.
