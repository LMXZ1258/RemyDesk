#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <rga/im2d.h>
#include <rga/rga.h>

#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>

struct capture_fb {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t pitch;
    uint32_t handle;
    uint32_t crtc_id;
};

static void fourcc_to_string(uint32_t fourcc, char out[5])
{
    out[0] = fourcc & 0xff;
    out[1] = (fourcc >> 8) & 0xff;
    out[2] = (fourcc >> 16) & 0xff;
    out[3] = (fourcc >> 24) & 0xff;
    out[4] = '\0';
}

static int rga_format_for_drm(uint32_t fourcc)
{
    switch (fourcc) {
    case DRM_FORMAT_XRGB8888: return RK_FORMAT_BGRX_8888;
    case DRM_FORMAT_ARGB8888: return RK_FORMAT_BGRA_8888;
    case DRM_FORMAT_ABGR8888: return RK_FORMAT_RGBA_8888;
    case DRM_FORMAT_XBGR8888: return RK_FORMAT_RGBX_8888;
    case DRM_FORMAT_RGB888: return RK_FORMAT_RGB_888;
    case DRM_FORMAT_BGR888: return RK_FORMAT_BGR_888;
    default: return 0;
    }
}

static drmModeEncoder *find_encoder_for_connector(int fd, drmModeConnector *conn)
{
    if (conn->encoder_id) return drmModeGetEncoder(fd, conn->encoder_id);
    for (int i = 0; i < conn->count_encoders; i++) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[i]);
        if (enc) return enc;
    }
    return NULL;
}

static int find_connected_hdmi_fb(int fd, struct capture_fb *out)
{
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        fprintf(stderr, "drmModeGetResources failed: %s\n", strerror(errno));
        return -1;
    }

    uint32_t crtc_id = 0;
    for (int i = 0; i < res->count_connectors && !crtc_id; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) continue;
        int is_hdmi = conn->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
                      conn->connector_type == DRM_MODE_CONNECTOR_HDMIB;
        if (is_hdmi && conn->connection == DRM_MODE_CONNECTED) {
            drmModeEncoder *enc = find_encoder_for_connector(fd, conn);
            if (enc) {
                crtc_id = enc->crtc_id;
                drmModeFreeEncoder(enc);
            }
        }
        drmModeFreeConnector(conn);
    }

    if (!crtc_id) {
        fprintf(stderr, "no connected HDMI CRTC found\n");
        drmModeFreeResources(res);
        return -1;
    }

    drmModeCrtc *crtc = drmModeGetCrtc(fd, crtc_id);
    if (!crtc || !crtc->buffer_id) {
        fprintf(stderr, "HDMI CRTC has no active framebuffer\n");
        if (crtc) drmModeFreeCrtc(crtc);
        drmModeFreeResources(res);
        return -1;
    }

    drmModeFB2Ptr fb = drmModeGetFB2(fd, crtc->buffer_id);
    if (!fb) {
        fprintf(stderr, "drmModeGetFB2(%u) failed: %s\n", crtc->buffer_id, strerror(errno));
        drmModeFreeCrtc(crtc);
        drmModeFreeResources(res);
        return -1;
    }

    if (!fb->handles[0]) {
        fprintf(stderr, "active fb %u has no GEM handle; run as root or DRM master\n", fb->fb_id);
        drmModeFreeFB2(fb);
        drmModeFreeCrtc(crtc);
        drmModeFreeResources(res);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->fb_id = fb->fb_id;
    out->width = fb->width;
    out->height = fb->height;
    out->fourcc = fb->pixel_format;
    out->pitch = fb->pitches[0];
    out->handle = fb->handles[0];
    out->crtc_id = crtc_id;

    drmModeFreeFB2(fb);
    drmModeFreeCrtc(crtc);
    drmModeFreeResources(res);
    return 0;
}

static int append_all(int fd, const void *data, size_t size)
{
    const uint8_t *ptr = data;
    size_t left = size;
    while (left) {
        ssize_t n = write(fd, ptr, left);
        if (n < 0) {
            fprintf(stderr, "write failed: %s\n", strerror(errno));
            return -1;
        }
        ptr += n;
        left -= (size_t)n;
    }
    return 0;
}

static int configure_encoder(MppCtx ctx, MppApi *mpi, uint32_t width, uint32_t height)
{
    MppEncCfg cfg = NULL;
    MPP_RET ret = mpp_enc_cfg_init(&cfg);
    if (ret) {
        fprintf(stderr, "mpp_enc_cfg_init failed %d\n", ret);
        return -1;
    }

    ret = mpi->control(ctx, MPP_ENC_GET_CFG, cfg);
    if (ret) {
        fprintf(stderr, "MPP_ENC_GET_CFG failed %d\n", ret);
        mpp_enc_cfg_deinit(cfg);
        return -1;
    }

    int fps = 30;
    int bitrate = 12000000;

    mpp_enc_cfg_set_s32(cfg, "prep:width", (RK_S32)width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", (RK_S32)height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", (RK_S32)width);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", (RK_S32)height);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);

    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bitrate);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bitrate);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bitrate);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", fps);

    mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
    mpp_enc_cfg_set_s32(cfg, "h264:level", 40);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);

    ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
    mpp_enc_cfg_deinit(cfg);
    if (ret) {
        fprintf(stderr, "MPP_ENC_SET_CFG failed %d\n", ret);
        return -1;
    }

    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    const char *card = argc > 1 ? argv[1] : "/dev/dri/card0";
    const char *out_path = argc > 2 ? argv[2] : "/tmp/drm-rga-mpp-frame.h264";

    int drm_fd = open(card, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", card, strerror(errno));
        return 1;
    }
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);

    struct capture_fb cap;
    if (find_connected_hdmi_fb(drm_fd, &cap) != 0) {
        close(drm_fd);
        return 1;
    }

    char fourcc[5];
    fourcc_to_string(cap.fourcc, fourcc);
    printf("capture fb=%u crtc=%u %ux%u fourcc=%s pitch=%u handle=%u\n",
           cap.fb_id, cap.crtc_id, cap.width, cap.height, fourcc, cap.pitch, cap.handle);

    int src_format = rga_format_for_drm(cap.fourcc);
    if (!src_format) {
        fprintf(stderr, "unsupported DRM format %s\n", fourcc);
        close(drm_fd);
        return 1;
    }

    int src_dma_fd = -1;
    if (drmPrimeHandleToFD(drm_fd, cap.handle, DRM_CLOEXEC, &src_dma_fd) != 0) {
        fprintf(stderr, "drmPrimeHandleToFD failed: %s\n", strerror(errno));
        close(drm_fd);
        return 1;
    }

    MppCtx ctx = NULL;
    MppApi *mpi = NULL;
    MPP_RET ret = mpp_create(&ctx, &mpi);
    if (ret || !ctx || !mpi) {
        fprintf(stderr, "mpp_create failed %d\n", ret);
        close(src_dma_fd);
        close(drm_fd);
        return 1;
    }
    printf("mpp_create ok\n");

    ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret) {
        fprintf(stderr, "mpp_init encoder failed %d\n", ret);
        mpp_destroy(ctx);
        close(src_dma_fd);
        close(drm_fd);
        return 1;
    }
    printf("mpp_init ok\n");

    RK_S64 timeout = 3000;
    mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);

    if (configure_encoder(ctx, mpi, cap.width, cap.height) != 0) {
        mpp_destroy(ctx);
        close(src_dma_fd);
        close(drm_fd);
        return 1;
    }
    printf("configure_encoder ok\n");

    size_t frame_size = (size_t)cap.width * cap.height * 3 / 2;
    MppBufferGroup group = NULL;
    MppBuffer mpp_buf = NULL;
    ret = mpp_buffer_group_get(&group, MPP_BUFFER_TYPE_DRM, MPP_BUFFER_INTERNAL, "drm_rga_mpp", "main");
    if (ret) {
        fprintf(stderr, "mpp_buffer_group_get failed %d\n", ret);
        mpp_destroy(ctx);
        close(src_dma_fd);
        close(drm_fd);
        return 1;
    }
    printf("mpp_buffer_group_get ok\n");
    ret = mpp_buffer_get(group, &mpp_buf, frame_size);
    if (ret || !mpp_buf) {
        fprintf(stderr, "mpp_buffer_get failed %d\n", ret);
        mpp_buffer_group_put(group);
        mpp_destroy(ctx);
        close(src_dma_fd);
        close(drm_fd);
        return 1;
    }
    printf("mpp_buffer_get ok fd=%d size=%zu\n", mpp_buffer_get_fd(mpp_buf), mpp_buffer_get_size(mpp_buf));

    int dst_dma_fd = mpp_buffer_get_fd(mpp_buf);
    if (dst_dma_fd < 0) {
        fprintf(stderr, "mpp_buffer_get_fd failed\n");
        mpp_buffer_put(mpp_buf);
        mpp_buffer_group_put(group);
        mpp_destroy(ctx);
        close(src_dma_fd);
        close(drm_fd);
        return 1;
    }

    int src_wstride = (int)(cap.pitch / 4);
    im_handle_param_t src_param = {
        .width = (uint32_t)src_wstride,
        .height = cap.height,
        .format = (uint32_t)src_format,
    };
    im_handle_param_t dst_param = {
        .width = cap.width,
        .height = cap.height,
        .format = RK_FORMAT_YCbCr_420_SP,
    };
    rga_buffer_handle_t src_handle = importbuffer_fd(src_dma_fd, &src_param);
    rga_buffer_handle_t dst_handle = importbuffer_fd(dst_dma_fd, &dst_param);
    if (!src_handle || !dst_handle) {
        fprintf(stderr, "RGA import failed src=%llu dst=%llu\n",
                (unsigned long long)src_handle, (unsigned long long)dst_handle);
        if (src_handle) releasebuffer_handle(src_handle);
        if (dst_handle) releasebuffer_handle(dst_handle);
        mpp_buffer_put(mpp_buf);
        mpp_buffer_group_put(group);
        mpp_destroy(ctx);
        close(src_dma_fd);
        close(drm_fd);
        return 1;
    }

    rga_buffer_t src = wrapbuffer_handle(src_handle, (int)cap.width, (int)cap.height,
                                         src_format, src_wstride, (int)cap.height);
    rga_buffer_t dst = wrapbuffer_handle(dst_handle, (int)cap.width, (int)cap.height,
                                         RK_FORMAT_YCbCr_420_SP, (int)cap.width, (int)cap.height);

    IM_STATUS status = imcheck(src, dst, (im_rect){0}, (im_rect){0});
    if (status != IM_STATUS_NOERROR) {
        fprintf(stderr, "RGA imcheck failed: %s\n", imStrError(status));
        releasebuffer_handle(src_handle);
        releasebuffer_handle(dst_handle);
        mpp_buffer_put(mpp_buf);
        mpp_buffer_group_put(group);
        mpp_destroy(ctx);
        close(src_dma_fd);
        close(drm_fd);
        return 1;
    }

    status = imcopy(src, dst);
    releasebuffer_handle(src_handle);
    releasebuffer_handle(dst_handle);
    close(src_dma_fd);
    close(drm_fd);
    if (status != IM_STATUS_SUCCESS) {
        fprintf(stderr, "RGA imcopy failed: %s\n", imStrError(status));
        mpp_buffer_put(mpp_buf);
        mpp_buffer_group_put(group);
        mpp_destroy(ctx);
        return 1;
    }
    printf("rga imcopy ok\n");

    MppFrame frame = NULL;
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, cap.width);
    mpp_frame_set_height(frame, cap.height);
    mpp_frame_set_hor_stride(frame, cap.width);
    mpp_frame_set_ver_stride(frame, cap.height);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, mpp_buf);

    int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out_fd < 0) {
        fprintf(stderr, "open output %s failed: %s\n", out_path, strerror(errno));
        mpp_frame_deinit(&frame);
        mpp_buffer_put(mpp_buf);
        mpp_buffer_group_put(group);
        mpp_destroy(ctx);
        return 1;
    }

    printf("encode_put_frame\n");
    ret = mpi->encode_put_frame(ctx, frame);
    mpp_frame_deinit(&frame);
    if (ret) {
        fprintf(stderr, "encode_put_frame failed %d\n", ret);
        close(out_fd);
        mpp_buffer_put(mpp_buf);
        mpp_buffer_group_put(group);
        mpp_destroy(ctx);
        return 1;
    }

    size_t total = 0;
    for (int i = 0; i < 8; i++) {
        MppPacket packet = NULL;
        printf("encode_get_packet try %d\n", i);
        ret = mpi->encode_get_packet(ctx, &packet);
        if (ret == MPP_ERR_TIMEOUT || !packet) {
            continue;
        }
        if (ret) {
            fprintf(stderr, "encode_get_packet failed %d\n", ret);
            close(out_fd);
            mpp_buffer_put(mpp_buf);
            mpp_buffer_group_put(group);
            mpp_destroy(ctx);
            return 1;
        }

        size_t len = mpp_packet_get_length(packet);
        if (len) {
            append_all(out_fd, mpp_packet_get_pos(packet), len);
            total += len;
        }
        mpp_packet_deinit(&packet);
        if (total) {
            break;
        }
    }

    close(out_fd);
    if (!total) {
        fprintf(stderr, "encoder returned no frame packet\n");
        mpp_buffer_put(mpp_buf);
        mpp_buffer_group_put(group);
        mpp_destroy(ctx);
        return 1;
    }

    printf("wrote %s frame_packet=%zu\n", out_path, total);
    mpp_buffer_put(mpp_buf);
    mpp_buffer_group_put(group);
    mpp_destroy(ctx);
    return 0;
}
