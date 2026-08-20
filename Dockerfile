# syntax=docker/dockerfile:1.7

ARG UBUNTU_VERSION=20.04
ARG GO_VERSION=1.26
ARG TARGETOS=linux
ARG TARGETARCH=arm64
ARG TARGETPLATFORM=linux/arm64
ARG BUILDPLATFORM=linux/arm64

FROM --platform=$BUILDPLATFORM golang:${GO_VERSION}-bookworm AS go-builder
ARG TARGETOS
ARG TARGETARCH
ARG GOPROXY=https://proxy.golang.org,direct
WORKDIR /src/native/webrtc-publisher
COPY native/webrtc-publisher/go.mod native/webrtc-publisher/go.sum ./
RUN GOPROXY="$GOPROXY" go mod download
RUN set -eu; \
    mkdir -p /out/go-licenses; \
    GOPROXY="$GOPROXY" go list -m \
      -f '{{if not .Main}}{{.Path}}|{{.Version}}|{{.Dir}}{{end}}' all \
      > /out/go-licenses/MODULES.txt; \
    while IFS='|' read -r module_path module_version module_dir; do \
      [ -n "$module_dir" ] || continue; \
      license_dir="/out/go-licenses/$(printf '%s@%s' "$module_path" "$module_version" | tr '/:' '__')"; \
      mkdir -p "$license_dir"; \
      for license in "$module_dir"/LICENSE* "$module_dir"/COPYING* "$module_dir"/NOTICE*; do \
        [ -f "$license" ] || continue; \
        cp "$license" "$license_dir/"; \
      done; \
    done < /out/go-licenses/MODULES.txt
COPY VERSION /src/VERSION
COPY native/webrtc-publisher/ ./
RUN GOPROXY="$GOPROXY" go test -trimpath ./... && \
    VERSION="$(tr -d '[:space:]' </src/VERSION)" && \
    CGO_ENABLED=0 GOOS=${TARGETOS} GOARCH=${TARGETARCH} \
    go build -trimpath -ldflags="-s -w -X main.version=${VERSION}" \
      -o /out/remydesk-webrtc-publisher .

FROM --platform=$TARGETPLATFORM ubuntu:${UBUNTU_VERSION} AS native-builder
ARG TARGETARCH
ARG DEBIAN_FRONTEND=noninteractive
WORKDIR /src
RUN test "$TARGETARCH" = arm64
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates cmake curl libdrm-dev pkg-config xz-utils && \
    rm -rf /var/lib/apt/lists/*

# Firefly's RK3399 MPP userspace is pinned to the same package used by the
# validated AIO-3399J Ubuntu 20.04 image. Checksums prevent repository drift.
ARG MPP_VERSION=1.5.0-1firefly2
ARG MPP_RUNTIME_SHA256=d42d9ac96bd4731b54ac58bc2d34d176b2ced80d1c55cdc9e4babde00e5bdf0c
ARG MPP_DEV_SHA256=f9d9a4e06fc3c3fd8f525e488ee8bb04068595b7f143120dabdda369053dc9c0
RUN curl -fL --retry 4 -o /tmp/mpp-runtime.deb \
      "http://wiki.t-firefly.com/firefly-ubuntu-repo/pool/main/m/mpp/librockchip-mpp1_${MPP_VERSION}_arm64.deb" && \
    curl -fL --retry 4 -o /tmp/mpp-dev.deb \
      "http://wiki.t-firefly.com/firefly-ubuntu-repo/pool/main/m/mpp/librockchip-mpp-dev_${MPP_VERSION}_arm64.deb" && \
    echo "${MPP_RUNTIME_SHA256}  /tmp/mpp-runtime.deb" | sha256sum -c - && \
    echo "${MPP_DEV_SHA256}  /tmp/mpp-dev.deb" | sha256sum -c - && \
    apt-get update && apt-get install -y --no-install-recommends \
      /tmp/mpp-runtime.deb /tmp/mpp-dev.deb && \
    rm -rf /var/lib/apt/lists/*

COPY CMakeLists.txt VERSION ./
COPY src/ src/
COPY third_party/ third_party/
COPY native/desktop-streamer/ native/desktop-streamer/
COPY scripts/fetch-rk3399-librga.sh scripts/fetch-rk3399-librga.sh
RUN scripts/fetch-rk3399-librga.sh third_party/librga/rk3399/librga.so.2
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF && \
    cmake --build build -j2 && \
    make -C native/desktop-streamer -j2 \
      RK3399_RGA_ROOT=/src/third_party/librga/rk3399 \
      drm_fb_probe drm_rga_mpp_stream

FROM --platform=$TARGETPLATFORM ubuntu:${UBUNTU_VERSION} AS runtime
ARG TARGETARCH
ARG DEBIAN_FRONTEND=noninteractive
LABEL org.opencontainers.image.title="RemyDesk for Firefly RK3399" \
      org.opencontainers.image.description="Low-latency WebRTC desktop and file management for Firefly AIO-3399J" \
      org.opencontainers.image.source="https://github.com/LMXZ1258/RemyDesk" \
      org.opencontainers.image.licenses="MIT"
RUN test "$TARGETARCH" = arm64
COPY --from=native-builder /tmp/mpp-runtime.deb /tmp/mpp-runtime.deb
RUN apt-get update && apt-get install -y --no-install-recommends \
      /tmp/mpp-runtime.deb \
      bash ca-certificates curl iproute2 iw libdrm2 libstdc++6 \
      network-manager pkg-config procps tini util-linux x11-xserver-utils xauth && \
    rm -f /tmp/mpp-runtime.deb && rm -rf /var/lib/apt/lists/*

COPY --from=native-builder /src/build/remydeskd /opt/remydesk/bin/remydeskd
COPY --from=go-builder /out/remydesk-webrtc-publisher /opt/remydesk/bin/remydesk-webrtc-publisher
COPY --from=native-builder /src/native/desktop-streamer/drm_fb_probe /opt/remydesk/libexec/drm_fb_probe
COPY --from=native-builder /src/native/desktop-streamer/drm_rga_mpp_stream /opt/remydesk/libexec/drm_rga_mpp_stream
COPY --from=native-builder /src/third_party/librga/rk3399/librga.so.2 /opt/remydesk/lib/rga-rk3399/librga.so.2
COPY native/desktop-streamer/drm_hotplug_stream.sh native/desktop-streamer/portable_h264_stream.sh /opt/remydesk/libexec/
COPY scripts/remydesk-display-mode.sh scripts/remydesk-doctor.sh scripts/detect-profile.sh /opt/remydesk/libexec/
COPY web/ /opt/remydesk/share/web/
COPY profiles/ /opt/remydesk/share/profiles/
COPY docker/remydesk-container-service docker/remydesk-entrypoint /opt/remydesk/bin/
COPY LICENSE THIRD_PARTY_NOTICES.md /usr/share/licenses/remydesk/
COPY --from=native-builder /src/third_party/librga/rk3399/LICENSE /usr/share/licenses/remydesk/librga-COPYING
COPY --from=go-builder /out/go-licenses/ /usr/share/licenses/remydesk/go-modules/

RUN ln -s librga.so.2 /opt/remydesk/lib/rga-rk3399/librga.so && \
    chmod 0755 /opt/remydesk/bin/* /opt/remydesk/libexec/*.sh && \
    mkdir -p /srv/remydesk/data /var/lib/remydesk /run/remydesk

ENV REMYDESK_HOST=0.0.0.0 \
    REMYDESK_PORT=8010 \
    REMYDESK_STORAGE_ROOT=/srv/remydesk/data \
    REMYDESK_STATE_DIR=/var/lib/remydesk \
    REMYDESK_RUNTIME_DIR=/run/remydesk \
    REMYDESK_HARDWARE_FILE=/var/lib/remydesk/hardware.json \
    REMYDESK_WEB_ROOT=/opt/remydesk/share/web \
    REMYDESK_SYSTEMCTL=/opt/remydesk/bin/remydesk-container-service \
    REMYDESK_DESKTOP_SERVICE=remydesk-desktop.service \
    REMYDESK_DESKTOP_PORT=8088 \
    REMYDESK_DESKTOP_AUTOSTART=1 \
    REMYDESK_AUTO_HOTSPOT=false \
    SIGNAL_MODE=lan \
    LAN_LISTEN=:8088 \
    SOURCE_MODE=command \
    H264_WORKDIR=/opt/remydesk/libexec \
    H264_COMMAND="exec ./portable_h264_stream.sh" \
    H264_TRANSPORT=shm \
    H264_SHM_DIR=/run/remydesk \
    H264_VARIABLE_FPS=true \
    H264_ADAPTIVE_BITRATE=1 \
    H264_BITRATE_MIN=2000000 \
    H264_BITRATE_MAX=8000000 \
    H264_CC_INTERVAL_MS=1000 \
    H264_STATS_LOG_SECONDS=30 \
    REMYDESK_VIDEO_BACKEND=native-mpp \
    REMYDESK_VIDEO_WIDTH=1920 \
    REMYDESK_VIDEO_HEIGHT=1080 \
    REMYDESK_VIDEO_FPS=30 \
    REMYDESK_VIDEO_BITRATE=6000000 \
    REMYDESK_H264_PROFILE=main \
    REMYDESK_H264_LEVEL=40 \
    REMYDESK_H264_CABAC=1 \
    H264_FMTP="level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d0028" \
    FPS=30 \
    REMYDESK_NATIVE_MPP_ZERO_COPY=1 \
    REMYDESK_NATIVE_BUFFER_COUNT=1 \
    REMYDESK_NATIVE_WAIT_VBLANK=0 \
    REMYDESK_MPP_LEGACY_FPS_KEYS=1 \
    REMYDESK_DYNAMIC_FPS=1 \
    REMYDESK_IDLE_FPS=8 \
    REMYDESK_IDLE_AFTER_MS=3000 \
    REMYDESK_IDLE_FRAME_BYTES=12000 \
    INPUT_ENABLED=true \
    INPUT_WIDTH=1920 \
    INPUT_HEIGHT=1080 \
    AUDIO_ENABLED=false \
    LD_LIBRARY_PATH=/opt/remydesk/lib/rga-rk3399 \
    HDMI_REQUIRED_MODE=1920x1080 \
    HDMI_FORCE_MODE=1 \
    HDMI_XRANDR_RATE=60 \
    HDMI_XRANDR_OUTPUT=auto \
    HDMI_DISPLAY=:0 \
    HDMI_XAUTHORITY=auto \
    REMYDESK_DRM_FORCE_CONNECTOR=1

VOLUME ["/srv/remydesk/data", "/var/lib/remydesk"]
EXPOSE 8010/tcp 8088/tcp 8088/udp
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8010/api/health >/dev/null || exit 1
ENTRYPOINT ["/usr/bin/tini", "--", "/opt/remydesk/bin/remydesk-entrypoint"]
