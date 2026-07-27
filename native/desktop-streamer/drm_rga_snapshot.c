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
    case DRM_FORMAT_XRGB8888:
        return RK_FORMAT_BGRX_8888;
    case DRM_FORMAT_ARGB8888:
        return RK_FORMAT_BGRA_8888;
    case DRM_FORMAT_ABGR8888:
        return RK_FORMAT_RGBA_8888;
    case DRM_FORMAT_XBGR8888:
        return RK_FORMAT_RGBX_8888;
    case DRM_FORMAT_RGB888:
        return RK_FORMAT_RGB_888;
    case DRM_FORMAT_BGR888:
        return RK_FORMAT_BGR_888;
    default:
        return 0;
    }
}

static drmModeEncoder *find_encoder_for_connector(int fd, drmModeConnector *conn)
{
    if (conn->encoder_id) {
        return drmModeGetEncoder(fd, conn->encoder_id);
    }

    for (int i = 0; i < conn->count_encoders; i++) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[i]);
        if (enc) {
            return enc;
        }
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
        if (!conn) {
            continue;
        }

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
        if (crtc) {
            drmModeFreeCrtc(crtc);
        }
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
        fprintf(stderr, "active fb %u has no exported GEM handle; run as root or DRM master\n", fb->fb_id);
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

static int write_all(const char *path, const void *data, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "open output %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    const uint8_t *ptr = data;
    size_t left = size;
    while (left) {
        ssize_t n = write(fd, ptr, left);
        if (n < 0) {
            fprintf(stderr, "write output failed: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
        ptr += n;
        left -= (size_t)n;
    }

    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    const char *card = argc > 1 ? argv[1] : "/dev/dri/card0";
    const char *out_path = argc > 2 ? argv[2] : "/tmp/drm-rga-frame.nv12";

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
        fprintf(stderr, "unsupported DRM format %s for RGA snapshot\n", fourcc);
        close(drm_fd);
        return 1;
    }

    int src_dma_fd = -1;
    if (drmPrimeHandleToFD(drm_fd, cap.handle, DRM_CLOEXEC, &src_dma_fd) != 0) {
        fprintf(stderr, "drmPrimeHandleToFD failed: %s\n", strerror(errno));
        close(drm_fd);
        return 1;
    }

    size_t dst_size = (size_t)cap.width * cap.height * 3 / 2;
    void *dst_mem = NULL;
    if (posix_memalign(&dst_mem, 64, dst_size) != 0 || !dst_mem) {
        fprintf(stderr, "failed to allocate destination buffer\n");
        close(src_dma_fd);
        close(drm_fd);
        return 1;
    }
    memset(dst_mem, 0, dst_size);

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
    rga_buffer_handle_t dst_handle = importbuffer_virtualaddr(dst_mem, &dst_param);
    if (!src_handle || !dst_handle) {
        fprintf(stderr, "RGA import failed: src_handle=%llu dst_handle=%llu\n",
                (unsigned long long)src_handle, (unsigned long long)dst_handle);
        if (src_handle) releasebuffer_handle(src_handle);
        if (dst_handle) releasebuffer_handle(dst_handle);
        free(dst_mem);
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
        free(dst_mem);
        close(src_dma_fd);
        close(drm_fd);
        return 1;
    }

    status = imcopy(src, dst);
    if (status != IM_STATUS_SUCCESS) {
        fprintf(stderr, "RGA imcopy failed: %s\n", imStrError(status));
        releasebuffer_handle(src_handle);
        releasebuffer_handle(dst_handle);
        free(dst_mem);
        close(src_dma_fd);
        close(drm_fd);
        return 1;
    }

    if (write_all(out_path, dst_mem, dst_size) == 0) {
        printf("wrote %s size=%zu format=NV12\n", out_path, dst_size);
    }

    releasebuffer_handle(src_handle);
    releasebuffer_handle(dst_handle);
    free(dst_mem);
    close(src_dma_fd);
    close(drm_fd);
    return 0;
}
