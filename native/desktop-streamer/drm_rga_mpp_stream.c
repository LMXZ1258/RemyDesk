#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/dma-buf.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <rga/im2d.h>
#include <rga/im2d_mpi.h>
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
    uint64_t modifier;
    uint32_t handle;
    uint32_t crtc_id;
};

struct src_import {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t pitch;
    uint64_t modifier;
    uint32_t handle_id;
    int dma_fd;
    void *map;
    size_t map_size;
    int rga_format;
    int wstride;
    rga_buffer_handle_t rga_handle;
    rga_buffer_t rga_buffer;
};

struct cursor_state {
    int active;
    uint32_t plane_id;
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t pitch;
    uint32_t handle;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

struct cursor_import {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t pitch;
    uint32_t handle_id;
    int dma_fd;
    int rga_format;
    int wstride;
    rga_buffer_handle_t rga_handle;
    rga_buffer_t rga_buffer;
};

struct dumb_rgb_buffer {
    uint32_t handle_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint64_t size;
    int dma_fd;
    void *map;
    int rga_format;
    int wstride;
    rga_buffer_handle_t rga_handle;
    rga_buffer_t rga_buffer;
};

struct encode_buffer {
    MppBuffer mpp_buf;
    int dma_fd;
    rga_buffer_handle_t rga_handle;
    rga_buffer_t rga_buffer;
};

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t force_idr_requested;

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static void on_idr_signal(int signo)
{
    (void)signo;
    force_idr_requested = 1;
}

static void fourcc_to_string(uint32_t fourcc, char out[5])
{
    out[0] = fourcc & 0xff;
    out[1] = (fourcc >> 8) & 0xff;
    out[2] = (fourcc >> 16) & 0xff;
    out[3] = (fourcc >> 24) & 0xff;
    out[4] = '\0';
    for (int i = 0; i < 4; i++) {
        if (out[i] < 32 || out[i] > 126) {
            out[i] = '.';
        }
    }
}

static int rga_format_for_drm(uint32_t fourcc, int xrgb_as_alpha)
{
    switch (fourcc) {
    case DRM_FORMAT_XRGB8888: return xrgb_as_alpha ? RK_FORMAT_BGRA_8888 : RK_FORMAT_BGRX_8888;
    case DRM_FORMAT_ARGB8888: return RK_FORMAT_BGRA_8888;
    case DRM_FORMAT_ABGR8888: return RK_FORMAT_RGBA_8888;
    case DRM_FORMAT_XBGR8888: return xrgb_as_alpha ? RK_FORMAT_RGBA_8888 : RK_FORMAT_RGBX_8888;
    case DRM_FORMAT_RGB888: return RK_FORMAT_RGB_888;
    case DRM_FORMAT_BGR888: return RK_FORMAT_BGR_888;
    default: return 0;
    }
}

static int bytes_per_pixel_for_drm(uint32_t fourcc)
{
    switch (fourcc) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
        return 4;
    case DRM_FORMAT_RGB888:
    case DRM_FORMAT_BGR888:
        return 3;
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

static int find_connected_display_fb(int fd, struct capture_fb *out)
{
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        fprintf(stderr, "drmModeGetResources failed: %s\n", strerror(errno));
        return -1;
    }

    uint32_t crtc_id = 0;
    /* Prefer HDMI for compatibility with the original RK3588 path, then
     * accept any connected connector with an active CRTC (DP/eDP/DSI). */
    for (int pass = 0; pass < 2 && !crtc_id; pass++) {
        for (int i = 0; i < res->count_connectors && !crtc_id; i++) {
            drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
            if (!conn) {
                continue;
            }

            int is_hdmi = conn->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
                          conn->connector_type == DRM_MODE_CONNECTOR_HDMIB;
            int wanted = pass == 0 ? is_hdmi : !is_hdmi;
            if (wanted && conn->connection == DRM_MODE_CONNECTED) {
                drmModeEncoder *enc = find_encoder_for_connector(fd, conn);
                if (enc) {
                    crtc_id = enc->crtc_id;
                    drmModeFreeEncoder(enc);
                }
            }
            drmModeFreeConnector(conn);
        }
    }

    if (!crtc_id) {
        fprintf(stderr, "no connected display CRTC found\n");
        drmModeFreeResources(res);
        return -1;
    }

    drmModeCrtc *crtc = drmModeGetCrtc(fd, crtc_id);
    if (!crtc || !crtc->buffer_id) {
        fprintf(stderr, "display CRTC has no active framebuffer\n");
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
    out->modifier = fb->modifier;
    out->handle = fb->handles[0];
    out->crtc_id = crtc_id;

    drmModeFreeFB2(fb);
    drmModeFreeCrtc(crtc);
    drmModeFreeResources(res);
    return 0;
}

static int crtc_index_for_id(int fd, uint32_t crtc_id)
{
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        return -1;
    }

    int index = -1;
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id) {
            index = i;
            break;
        }
    }
    drmModeFreeResources(res);
    return index;
}

static int wait_for_crtc_vblank(int fd, uint32_t crtc_id)
{
    int crtc_index = crtc_index_for_id(fd, crtc_id);
    if (crtc_index < 0) {
        return -1;
    }

    drmVBlank vbl;
    memset(&vbl, 0, sizeof(vbl));
    vbl.request.type = DRM_VBLANK_RELATIVE;
    vbl.request.sequence = 1;

    if (crtc_index == 1) {
        vbl.request.type |= DRM_VBLANK_SECONDARY;
    }
#ifdef DRM_VBLANK_HIGH_CRTC_SHIFT
    else if (crtc_index > 1) {
        vbl.request.type |= (drmVBlankSeqType)(crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT);
    }
#endif

    return drmWaitVBlank(fd, &vbl);
}

static int64_t drm_prop_to_s32(uint64_t value)
{
    return (int32_t)(uint32_t)value;
}

static int get_object_property(int fd, uint32_t object_id, uint32_t object_type,
                               const char *name, uint64_t *value)
{
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, object_id, object_type);
    if (!props) {
        return -1;
    }

    int found = -1;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop) {
            continue;
        }
        if (strcmp(prop->name, name) == 0) {
            *value = props->prop_values[i];
            found = 0;
            drmModeFreeProperty(prop);
            break;
        }
        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);
    return found;
}

static int get_plane_property_u64(int fd, uint32_t plane_id, const char *name, uint64_t *value)
{
    return get_object_property(fd, plane_id, DRM_MODE_OBJECT_PLANE, name, value);
}

static int find_cursor_plane(int fd, uint32_t crtc_id, struct cursor_state *out)
{
    memset(out, 0, sizeof(*out));

    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (!planes) {
        return -1;
    }

    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(fd, planes->planes[i]);
        if (!plane) {
            continue;
        }

        uint64_t plane_type = 0;
        int is_cursor = get_plane_property_u64(fd, plane->plane_id, "type", &plane_type) == 0 &&
                        plane_type == DRM_PLANE_TYPE_CURSOR;
        if (!is_cursor || plane->crtc_id != crtc_id || !plane->fb_id) {
            drmModeFreePlane(plane);
            continue;
        }

        drmModeFB2Ptr fb = drmModeGetFB2(fd, plane->fb_id);
        if (!fb || !fb->handles[0]) {
            if (fb) {
                drmModeFreeFB2(fb);
            }
            drmModeFreePlane(plane);
            continue;
        }

        uint64_t crtc_x = 0;
        uint64_t crtc_y = 0;
        uint64_t crtc_w = fb->width;
        uint64_t crtc_h = fb->height;
        get_plane_property_u64(fd, plane->plane_id, "CRTC_X", &crtc_x);
        get_plane_property_u64(fd, plane->plane_id, "CRTC_Y", &crtc_y);
        get_plane_property_u64(fd, plane->plane_id, "CRTC_W", &crtc_w);
        get_plane_property_u64(fd, plane->plane_id, "CRTC_H", &crtc_h);

        out->active = 1;
        out->plane_id = plane->plane_id;
        out->fb_id = fb->fb_id;
        out->width = fb->width;
        out->height = fb->height;
        out->fourcc = fb->pixel_format;
        out->pitch = fb->pitches[0];
        out->handle = fb->handles[0];
        out->x = (int32_t)drm_prop_to_s32(crtc_x);
        out->y = (int32_t)drm_prop_to_s32(crtc_y);
        out->w = crtc_w ? (int32_t)crtc_w : (int32_t)fb->width;
        out->h = crtc_h ? (int32_t)crtc_h : (int32_t)fb->height;

        drmModeFreeFB2(fb);
        drmModeFreePlane(plane);
        drmModeFreePlaneResources(planes);
        return 0;
    }

    drmModeFreePlaneResources(planes);
    return 0;
}

static void warn_active_overlay_planes(int fd, uint32_t crtc_id, uint32_t primary_fb_id)
{
    static int warned_overlay;
    if (warned_overlay) {
        return;
    }

    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (!planes) {
        return;
    }

    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(fd, planes->planes[i]);
        if (!plane) {
            continue;
        }
        if (plane->crtc_id != crtc_id || !plane->fb_id || plane->fb_id == primary_fb_id) {
            drmModeFreePlane(plane);
            continue;
        }

        uint64_t plane_type = 0;
        const char *type_name = "unknown";
        if (get_plane_property_u64(fd, plane->plane_id, "type", &plane_type) == 0) {
            if (plane_type == DRM_PLANE_TYPE_PRIMARY) {
                type_name = "primary";
            } else if (plane_type == DRM_PLANE_TYPE_CURSOR) {
                type_name = "cursor";
            } else if (plane_type == DRM_PLANE_TYPE_OVERLAY) {
                type_name = "overlay";
            }
        }
        if (plane_type != DRM_PLANE_TYPE_CURSOR) {
            fprintf(stderr,
                    "warning: active non-primary DRM %s plane detected plane=%u fb=%u; "
                    "this stream captures the primary framebuffer and cursor only, "
                    "so hardware video overlays can appear stale or partially old\n",
                    type_name, plane->plane_id, plane->fb_id);
            warned_overlay = 1;
            drmModeFreePlane(plane);
            break;
        }
        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(planes);
}

static void release_src_import(struct src_import *src)
{
    if (src->rga_handle) {
        releasebuffer_handle(src->rga_handle);
    }
    if (src->map && src->map != MAP_FAILED) {
        munmap(src->map, src->map_size);
    }
    if (src->dma_fd >= 0) {
        close(src->dma_fd);
    }
    memset(src, 0, sizeof(*src));
    src->dma_fd = -1;
}

static int ensure_src_import(int drm_fd, struct src_import *src,
                             const struct capture_fb *cap,
                             int cpu_stage,
                             int xrgb_as_alpha)
{
    if (src->dma_fd >= 0 &&
        src->fb_id == cap->fb_id &&
        src->handle_id == cap->handle &&
        src->width == cap->width &&
        src->height == cap->height &&
        src->fourcc == cap->fourcc &&
        src->pitch == cap->pitch &&
        src->modifier == cap->modifier) {
        return 0;
    }

    release_src_import(src);

    int rga_format = rga_format_for_drm(cap->fourcc, xrgb_as_alpha);
    int bytes_per_pixel = bytes_per_pixel_for_drm(cap->fourcc);
    if (!rga_format || !bytes_per_pixel) {
        char fourcc[5];
        fourcc_to_string(cap->fourcc, fourcc);
        fprintf(stderr, "unsupported DRM format %s\n", fourcc);
        return -1;
    }

    int dma_fd = -1;
    if (drmPrimeHandleToFD(drm_fd, cap->handle, DRM_CLOEXEC | DRM_RDWR, &dma_fd) != 0) {
        fprintf(stderr, "drmPrimeHandleToFD failed: %s\n", strerror(errno));
        return -1;
    }

    int wstride = (int)(cap->pitch / (uint32_t)bytes_per_pixel);
    im_handle_param_t src_param = {
        .width = (uint32_t)wstride,
        .height = cap->height,
        .format = (uint32_t)rga_format,
    };

    rga_buffer_handle_t rga_handle = importbuffer_fd(dma_fd, &src_param);
    if (!rga_handle) {
        fprintf(stderr, "RGA source import failed for fb=%u fd=%d\n", cap->fb_id, dma_fd);
        close(dma_fd);
        return -1;
    }

    src->fb_id = cap->fb_id;
    src->width = cap->width;
    src->height = cap->height;
    src->fourcc = cap->fourcc;
    src->pitch = cap->pitch;
    src->modifier = cap->modifier;
    src->handle_id = cap->handle;
    src->dma_fd = dma_fd;
    src->map = NULL;
    src->map_size = 0;
    src->rga_format = rga_format;
    src->wstride = wstride;
    src->rga_handle = rga_handle;
    src->rga_buffer = wrapbuffer_handle(rga_handle, (int)cap->width, (int)cap->height,
                                        rga_format, wstride, (int)cap->height);
    if (cpu_stage) {
        size_t map_size = (size_t)cap->pitch * cap->height;
        void *map = mmap(NULL, map_size, PROT_READ, MAP_SHARED, dma_fd, 0);
        if (map == MAP_FAILED) {
            fprintf(stderr, "mmap source fb failed: %s\n", strerror(errno));
            release_src_import(src);
            return -1;
        }
        src->map = map;
        src->map_size = map_size;
    }
    return 0;
}

static void release_cursor_import(struct cursor_import *cursor)
{
    if (cursor->rga_handle) {
        releasebuffer_handle(cursor->rga_handle);
    }
    if (cursor->dma_fd >= 0) {
        close(cursor->dma_fd);
    }
    memset(cursor, 0, sizeof(*cursor));
    cursor->dma_fd = -1;
}

static int ensure_cursor_import(int drm_fd, struct cursor_import *cursor,
                                const struct cursor_state *state)
{
    if (!state->active) {
        release_cursor_import(cursor);
        return 0;
    }

    if (cursor->dma_fd >= 0 &&
        cursor->fb_id == state->fb_id &&
        cursor->handle_id == state->handle &&
        cursor->width == state->width &&
        cursor->height == state->height &&
        cursor->fourcc == state->fourcc &&
        cursor->pitch == state->pitch) {
        return 0;
    }

    release_cursor_import(cursor);

    int rga_format = rga_format_for_drm(state->fourcc, 0);
    int bytes_per_pixel = bytes_per_pixel_for_drm(state->fourcc);
    if (!rga_format || !bytes_per_pixel) {
        char fourcc[5];
        fourcc_to_string(state->fourcc, fourcc);
        fprintf(stderr, "unsupported cursor DRM format %s\n", fourcc);
        return -1;
    }

    int dma_fd = -1;
    if (drmPrimeHandleToFD(drm_fd, state->handle, DRM_CLOEXEC | DRM_RDWR, &dma_fd) != 0) {
        fprintf(stderr, "cursor drmPrimeHandleToFD failed: %s\n", strerror(errno));
        return -1;
    }

    int wstride = (int)(state->pitch / (uint32_t)bytes_per_pixel);
    im_handle_param_t param = {
        .width = (uint32_t)wstride,
        .height = state->height,
        .format = (uint32_t)rga_format,
    };
    rga_buffer_handle_t rga_handle = importbuffer_fd(dma_fd, &param);
    if (!rga_handle) {
        fprintf(stderr, "RGA cursor import failed for fb=%u fd=%d\n", state->fb_id, dma_fd);
        close(dma_fd);
        return -1;
    }

    cursor->fb_id = state->fb_id;
    cursor->width = state->width;
    cursor->height = state->height;
    cursor->fourcc = state->fourcc;
    cursor->pitch = state->pitch;
    cursor->handle_id = state->handle;
    cursor->dma_fd = dma_fd;
    cursor->rga_format = rga_format;
    cursor->wstride = wstride;
    cursor->rga_handle = rga_handle;
    cursor->rga_buffer = wrapbuffer_handle(rga_handle, (int)state->width, (int)state->height,
                                           rga_format, wstride, (int)state->height);
    return 0;
}

static int blend_cursor_with_rga(struct cursor_import *cursor,
                                 const struct cursor_state *state,
                                 rga_buffer_t rgb,
                                 uint32_t screen_width,
                                 uint32_t screen_height)
{
    if (!state->active || cursor->dma_fd < 0 || state->w <= 0 || state->h <= 0) {
        return 0;
    }

    int src_x = 0;
    int src_y = 0;
    int dst_x = state->x;
    int dst_y = state->y;
    int width = state->w;
    int height = state->h;

    if (dst_x < 0) {
        src_x -= dst_x;
        width += dst_x;
        dst_x = 0;
    }
    if (dst_y < 0) {
        src_y -= dst_y;
        height += dst_y;
        dst_y = 0;
    }
    if (dst_x + width > (int)screen_width) {
        width = (int)screen_width - dst_x;
    }
    if (dst_y + height > (int)screen_height) {
        height = (int)screen_height - dst_y;
    }
    if (width <= 0 || height <= 0) {
        return 0;
    }

    if (src_x + width > (int)state->width) {
        width = (int)state->width - src_x;
    }
    if (src_y + height > (int)state->height) {
        height = (int)state->height - src_y;
    }
    if (width <= 0 || height <= 0) {
        return 0;
    }

    /*
     * Some RGA drivers reject cursor blend rectangles with odd source offsets
     * or odd dimensions, especially when the cursor is partially offscreen.
     * Expand/shrink the tiny cursor rect to even-aligned values instead of
     * stopping the video stream.
     */
    if (src_x & 1) {
        src_x--;
        width++;
    }
    if (src_y & 1) {
        src_y--;
        height++;
    }
    if (src_x + width > (int)state->width) {
        width = (int)state->width - src_x;
    }
    if (src_y + height > (int)state->height) {
        height = (int)state->height - src_y;
    }
    if (width & 1) {
        if (src_x + width < (int)state->width &&
            dst_x + width < (int)screen_width) {
            width++;
        } else {
            width--;
        }
    }
    if (height & 1) {
        if (src_y + height < (int)state->height &&
            dst_y + height < (int)screen_height) {
            height++;
        } else {
            height--;
        }
    }
    if (width < 2 || height < 2) {
        return 0;
    }

    im_rect src_rect = { src_x, src_y, width, height };
    im_rect dst_rect = { dst_x, dst_y, width, height };
    im_rect empty_rect = { 0, 0, 0, 0 };
    rga_buffer_t empty = { 0 };

    IM_STATUS status = improcess(cursor->rga_buffer, rgb, empty,
                                 src_rect, dst_rect, empty_rect,
                                 IM_ALPHA_BLEND_SRC_OVER | IM_SYNC);
    if (status != IM_STATUS_SUCCESS) {
        static int warned;
        if (!warned) {
            fprintf(stderr, "RGA cursor blend skipped: %s\n", imStrError(status));
            warned = 1;
        }
        return 0;
    }
    return 0;
}

static void release_dumb_rgb_buffer(int drm_fd, struct dumb_rgb_buffer *buf)
{
    if (buf->rga_handle) {
        releasebuffer_handle(buf->rga_handle);
    }
    if (buf->map && buf->map != MAP_FAILED) {
        munmap(buf->map, buf->size);
    }
    if (buf->dma_fd >= 0) {
        close(buf->dma_fd);
    }
    if (buf->handle_id) {
        struct drm_mode_destroy_dumb destroy_req;
        memset(&destroy_req, 0, sizeof(destroy_req));
        destroy_req.handle = buf->handle_id;
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
    }
    memset(buf, 0, sizeof(*buf));
    buf->dma_fd = -1;
}

static int create_dumb_rgb_buffer(int drm_fd, struct dumb_rgb_buffer *buf,
                                  uint32_t width, uint32_t height,
                                  int rga_format, int bytes_per_pixel)
{
    memset(buf, 0, sizeof(*buf));
    buf->dma_fd = -1;

    struct drm_mode_create_dumb create_req;
    memset(&create_req, 0, sizeof(create_req));
    create_req.width = width;
    create_req.height = height;
    create_req.bpp = (uint32_t)(bytes_per_pixel * 8);

    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) != 0) {
        fprintf(stderr, "DRM create dumb RGB buffer failed: %s\n", strerror(errno));
        return -1;
    }

    int dma_fd = -1;
    if (drmPrimeHandleToFD(drm_fd, create_req.handle, DRM_CLOEXEC | DRM_RDWR, &dma_fd) != 0) {
        fprintf(stderr, "DRM export dumb RGB buffer failed: %s\n", strerror(errno));
        struct drm_mode_destroy_dumb destroy_req;
        memset(&destroy_req, 0, sizeof(destroy_req));
        destroy_req.handle = create_req.handle;
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
        return -1;
    }

    int wstride = (int)(create_req.pitch / (uint32_t)bytes_per_pixel);
    im_handle_param_t param = {
        .width = (uint32_t)wstride,
        .height = height,
        .format = (uint32_t)rga_format,
    };
    rga_buffer_handle_t rga_handle = importbuffer_fd(dma_fd, &param);
    if (!rga_handle) {
        fprintf(stderr, "RGA dumb RGB import failed fd=%d pitch=%u\n", dma_fd, create_req.pitch);
        close(dma_fd);
        struct drm_mode_destroy_dumb destroy_req;
        memset(&destroy_req, 0, sizeof(destroy_req));
        destroy_req.handle = create_req.handle;
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
        return -1;
    }

    struct drm_mode_map_dumb map_req;
    memset(&map_req, 0, sizeof(map_req));
    map_req.handle = create_req.handle;
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) != 0) {
        fprintf(stderr, "DRM map dumb RGB buffer failed: %s\n", strerror(errno));
        releasebuffer_handle(rga_handle);
        close(dma_fd);
        struct drm_mode_destroy_dumb destroy_req;
        memset(&destroy_req, 0, sizeof(destroy_req));
        destroy_req.handle = create_req.handle;
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
        return -1;
    }

    void *map = mmap(NULL, create_req.size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, drm_fd, map_req.offset);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap dumb RGB buffer failed: %s\n", strerror(errno));
        releasebuffer_handle(rga_handle);
        close(dma_fd);
        struct drm_mode_destroy_dumb destroy_req;
        memset(&destroy_req, 0, sizeof(destroy_req));
        destroy_req.handle = create_req.handle;
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
        return -1;
    }

    buf->handle_id = create_req.handle;
    buf->width = width;
    buf->height = height;
    buf->pitch = create_req.pitch;
    buf->size = create_req.size;
    buf->dma_fd = dma_fd;
    buf->map = map;
    buf->rga_format = rga_format;
    buf->wstride = wstride;
    buf->rga_handle = rga_handle;
    buf->rga_buffer = wrapbuffer_handle(rga_handle, (int)width, (int)height,
                                        rga_format, wstride, (int)height);
    return 0;
}

static int copy_fb_to_rgb_stage(const struct src_import *src,
                                struct dumb_rgb_buffer *stage,
                                uint32_t width,
                                uint32_t height,
                                int bytes_per_pixel)
{
    if (!src->map || !stage->map) {
        fprintf(stderr, "cpu stage requested without mapped buffers\n");
        return -1;
    }

    size_t row_bytes = (size_t)width * (size_t)bytes_per_pixel;
    if (row_bytes > src->pitch || row_bytes > stage->pitch) {
        fprintf(stderr, "cpu stage row size exceeds source or stage pitch\n");
        return -1;
    }

    const uint8_t *src_row = (const uint8_t *)src->map;
    uint8_t *dst_row = (uint8_t *)stage->map;
    for (uint32_t y = 0; y < height; y++) {
        memcpy(dst_row, src_row, row_bytes);
        if (src->rga_format == RK_FORMAT_BGRX_8888 &&
            stage->rga_format == RK_FORMAT_BGRA_8888 &&
            bytes_per_pixel == 4) {
            for (uint32_t x = 0; x < width; x++) {
                dst_row[x * 4 + 3] = 0xff;
            }
        }
        src_row += src->pitch;
        dst_row += stage->pitch;
    }
    return 0;
}

static int write_all(int fd, const void *data, size_t size)
{
    const uint8_t *ptr = data;
    size_t left = size;
    while (left) {
        ssize_t n = write(fd, ptr, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EPIPE) {
                stop_requested = 1;
                return -1;
            }
            fprintf(stderr, "write failed: %s\n", strerror(errno));
            return -1;
        }
        ptr += n;
        left -= (size_t)n;
    }
    return 0;
}

static int dma_buf_publish(int fd)
{
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW };
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
        return -1;
    }
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static void add_ns(struct timespec *ts, long ns)
{
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

static int parse_u32_arg(const char *value, uint32_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno || !end || *end || parsed > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)parsed;
    return 0;
}

static int parse_h264_profile_arg(const char *value, int *profile, const char **name)
{
    if (strcmp(value, "baseline") == 0 || strcmp(value, "constrained-baseline") == 0) {
        *profile = 66;
        *name = "baseline";
        return 0;
    }
    if (strcmp(value, "main") == 0) {
        *profile = 77;
        *name = "main";
        return 0;
    }
    if (strcmp(value, "high") == 0) {
        *profile = 100;
        *name = "high";
        return 0;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 66 || parsed > 244) {
        return -1;
    }
    *profile = (int)parsed;
    *name = "custom";
    return 0;
}

static int rga_convert_frame(rga_buffer_t src, rga_buffer_t dst,
                             uint32_t src_width, uint32_t src_height,
                             uint32_t dst_width, uint32_t dst_height,
                             const char *label)
{
    im_rect src_rect = { 0, 0, (int)src_width, (int)src_height };
    im_rect dst_rect = { 0, 0, (int)dst_width, (int)dst_height };
    im_rect empty_rect = { 0, 0, 0, 0 };
    rga_buffer_t empty = { 0 };

    if (src_width == dst_width && src_height == dst_height &&
        src.format != dst.format) {
        IM_STATUS status = imcvtcolor(src, dst, src.format, dst.format,
                                      src.color_space_mode, 1);
        if (status == IM_STATUS_SUCCESS) {
            return 0;
        }
    }

    int cores[] = {
        IM_SCHEDULER_RGA2_CORE0,
        IM_SCHEDULER_RGA3_CORE0,
        IM_SCHEDULER_RGA3_CORE1,
    };

    IM_STATUS last_status = IM_STATUS_FAILED;
    for (size_t i = 0; i < sizeof(cores) / sizeof(cores[0]); i++) {
        im_opt_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.version = RGA_CURRENT_API_HEADER_VERSION;
        opt.core = cores[i];

        last_status = improcess_ctx(src, dst, empty,
                                    src_rect, dst_rect, empty_rect,
                                    -1, NULL, &opt, IM_SYNC, 0);
        if (last_status == IM_STATUS_SUCCESS) {
            return 0;
        }
    }

    fprintf(stderr, "%s failed: %s\n", label, imStrError(last_status));
    return -1;
}

static int configure_encoder(MppCtx ctx, MppApi *mpi,
                             uint32_t width, uint32_t height,
                             int fps, int bitrate,
                             MppFrameFormat mpp_format,
                             int h264_profile, int h264_level,
                             int h264_cabac)
{
    uint32_t ver_stride = (height + 15U) & ~15U;
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

    mpp_enc_cfg_set_s32(cfg, "prep:width", (RK_S32)width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", (RK_S32)height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", (RK_S32)width);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", (RK_S32)ver_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:format", mpp_format);

    /* Match the known-good vendor mpi_enc_test rate-control envelope.  This
     * old rk3588 MPP build emits damaged slices with the degenerate CBR
     * range min == target == max. */
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_VBR);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_thd", 20);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_gap", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bitrate);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bitrate * 17 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bitrate / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    /* Periodic IDR generation is broken in this vendor MPP build and causes
     * Chromium to freeze at the GOP boundary.  A fresh encoder is created for
     * every viewer, so it always starts with a clean IDR; keep that session's
     * reference chain continuous for up to one hour. */
    mpp_enc_cfg_set_s32(cfg, "rc:gop", getenv("REMYDESK_ALL_INTRA") ? 1 : fps * 3600);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_init", -1);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 51);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 10);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", 51);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", 10);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 2);

    mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg, "h264:profile", h264_profile);
    mpp_enc_cfg_set_s32(cfg, "h264:level", h264_level);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", h264_cabac);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", h264_profile >= 100 ? 1 : 0);

    ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
    mpp_enc_cfg_deinit(cfg);
    if (ret) {
        fprintf(stderr, "MPP_ENC_SET_CFG failed %d\n", ret);
        return -1;
    }

    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_DEFAULT;
    mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
    RK_U32 sei_mode = MPP_ENC_SEI_MODE_DISABLE;
    mpi->control(ctx, MPP_ENC_SET_SEI_CFG, &sei_mode);
    return 0;
}

static void release_encode_buffers(struct encode_buffer *buffers, int count)
{
    if (!buffers) {
        return;
    }
    for (int i = 0; i < count; i++) {
        if (buffers[i].rga_handle) {
            releasebuffer_handle(buffers[i].rga_handle);
        }
        if (buffers[i].mpp_buf) {
            mpp_buffer_put(buffers[i].mpp_buf);
        }
        memset(&buffers[i], 0, sizeof(buffers[i]));
        buffers[i].dma_fd = -1;
    }
}

static int init_encode_buffers(MppBufferGroup group,
                               struct encode_buffer *buffers,
                               int count,
                               size_t frame_size,
                               uint32_t width,
                               uint32_t height,
                               int dst_rga_format)
{
    uint32_t ver_stride = (height + 15U) & ~15U;
    for (int i = 0; i < count; i++) {
        buffers[i].dma_fd = -1;

        MPP_RET ret = mpp_buffer_get(group, &buffers[i].mpp_buf, frame_size);
        if (ret || !buffers[i].mpp_buf) {
            fprintf(stderr, "mpp_buffer_get[%d] failed %d\n", i, ret);
            return -1;
        }

        buffers[i].dma_fd = mpp_buffer_get_fd(buffers[i].mpp_buf);
        if (buffers[i].dma_fd < 0) {
            fprintf(stderr, "mpp_buffer_get_fd[%d] failed\n", i);
            return -1;
        }

        im_handle_param_t dst_param = {
            .width = width,
            .height = height,
            .format = (uint32_t)dst_rga_format,
        };
        buffers[i].rga_handle = importbuffer_fd(buffers[i].dma_fd, &dst_param);
        if (!buffers[i].rga_handle) {
            fprintf(stderr, "RGA destination import[%d] failed fd=%d\n", i, buffers[i].dma_fd);
            return -1;
        }

        buffers[i].rga_buffer = wrapbuffer_handle(buffers[i].rga_handle,
                                                  (int)width, (int)height,
                                                  dst_rga_format,
                                                  (int)width, (int)ver_stride);
    }
    return 0;
}

static int annexb_has_vcl(const uint8_t *data, size_t len)
{
    if (!data || len < 4) {
        return 0;
    }
    for (size_t i = 0; i + 3 < len; i++) {
        size_t nal = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            nal = i + 3;
        } else if (i + 4 < len && data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            nal = i + 4;
        }
        if (nal && nal < len) {
            unsigned type = data[nal] & 0x1fU;
            if (type >= 1 && type <= 5) {
                return 1;
            }
        }
    }
    return 0;
}

static int encode_one_frame(MppCtx ctx, MppApi *mpi, MppBuffer mpp_buf,
                            MppBuffer pkt_buf,
                            uint32_t width, uint32_t height,
                            MppFrameFormat mpp_format,
                            int64_t pts_us, int out_fd)
{
    uint32_t ver_stride = (height + 15U) & ~15U;
    MppFrame frame = NULL;
    MPP_RET ret = mpp_frame_init(&frame);
    if (ret || !frame) {
        fprintf(stderr, "mpp_frame_init failed %d\n", ret);
        return -1;
    }

    mpp_frame_set_width(frame, width);
    mpp_frame_set_height(frame, height);
    mpp_frame_set_hor_stride(frame, width);
    mpp_frame_set_ver_stride(frame, ver_stride);
    mpp_frame_set_fmt(frame, mpp_format);
    mpp_frame_set_pts(frame, pts_us);
    mpp_frame_set_buffer(frame, mpp_buf);
    mpp_frame_set_eos(frame, 0);

    MppPacket packet = NULL;
    ret = mpp_packet_init_with_buffer(&packet, pkt_buf);
    if (ret || !packet) {
        fprintf(stderr, "mpp_packet_init_with_buffer failed %d\n", ret);
        mpp_frame_deinit(&frame);
        return -1;
    }
    mpp_packet_set_length(packet, 0);
    mpp_meta_set_packet(mpp_frame_get_meta(frame), KEY_OUTPUT_PACKET, packet);

    if (force_idr_requested) {
        RK_S32 idr = 1;
        force_idr_requested = 0;
        ret = mpi->control(ctx, MPP_ENC_SET_IDR_FRAME, &idr);
        if (ret) {
            fprintf(stderr, "MPP_ENC_SET_IDR_FRAME failed %d\n", ret);
        }
    }

    ret = mpi->encode_put_frame(ctx, frame);
    mpp_frame_deinit(&frame);
    if (ret) {
        fprintf(stderr, "encode_put_frame failed %d\n", ret);
        mpp_packet_deinit(&packet);
        return -1;
    }

    for (int i = 0; i < 128 && !stop_requested; i++) {
        ret = mpi->encode_get_packet(ctx, &packet);
        if (ret == MPP_ERR_TIMEOUT || !packet) {
            usleep(1000);
            continue;
        }
        if (ret) {
            fprintf(stderr, "encode_get_packet failed %d\n", ret);
            return -1;
        }

        size_t len = mpp_packet_get_length(packet);
        void *pos = mpp_packet_get_pos(packet);
        RK_U32 is_partition = mpp_packet_is_partition(packet);
        RK_U32 is_eoi = mpp_packet_is_eoi(packet);
        int has_vcl = annexb_has_vcl((const uint8_t *)pos, len);
        if (getenv("REMYDESK_PACKET_DEBUG")) {
            fprintf(stderr,
                    "packet pts=%lld index=%d len=%zu partition=%u eoi=%u vcl=%d\n",
                    (long long)pts_us, i, len, is_partition, is_eoi, has_vcl);
        }
        int write_rc = 0;
        if (len) {
            write_rc = write_all(out_fd, pos, len);
        }
        mpp_packet_deinit(&packet);
        if (write_rc != 0) {
            return -1;
        }

        /*
         * Rockchip MPP may return one encoded frame as several packet
         * partitions.  Draining only the first packet leaves the remaining
         * partitions queued, so the next input frame receives stale data and
         * the Annex-B stream quickly becomes corrupt.
         */
        if (is_eoi || !is_partition) {
            return 0;
        }
    }

    if (packet) {
        mpp_packet_deinit(&packet);
    }
    if (!stop_requested) {
        fprintf(stderr, "encode_get_packet did not finish a frame\n");
    }
    return stop_requested ? 0 : -1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-c /dev/dri/card0] [-f fps] [-b bitrate] [-n frames] [-q]\n"
            "          [--no-cursor] [--cpu-stage] [--xrgb-as-alpha] [--wait-vblank]\n"
            "          [--buffer-count 1..8]\n"
            "          [--color default|bt709|bt601] [--yuv nv12|nv21]\n"
            "          [--out-width pixels] [--out-height pixels]\n"
            "          [--h264-profile baseline|main|high|66|77|100] [--h264-level 40]\n"
            "          [--h264-cabac 0|1]\n"
            "\n"
            "Outputs Annex-B H.264 to stdout. Status and errors go to stderr.\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *card = "/dev/dri/card0";
    int fps = 30;
    int bitrate = 12000000;
    int max_frames = 0;
    int quiet = 0;
    int cursor_enabled = 1;
    int cpu_stage = 0;
    int xrgb_as_alpha = 0;
    int wait_vblank = 1;
    int buffer_count = 3;
    uint32_t out_width = 0;
    uint32_t out_height = 0;
    int color_mode = IM_COLOR_SPACE_DEFAULT;
    const char *color_name = "default";
    int dst_rga_format = RK_FORMAT_YCbCr_420_SP;
    MppFrameFormat mpp_format = MPP_FMT_YUV420SP;
    const char *yuv_name = "nv12";
    int h264_profile = 100;
    const char *h264_profile_name = "high";
    int h264_level = 40;
    int h264_cabac = 1;

    int opt;
    enum {
        OPT_NO_CURSOR = 1000,
        OPT_COLOR,
        OPT_YUV,
        OPT_OUT_WIDTH,
        OPT_OUT_HEIGHT,
        OPT_H264_PROFILE,
        OPT_H264_LEVEL,
        OPT_H264_CABAC,
        OPT_CPU_STAGE,
        OPT_XRGB_AS_ALPHA,
        OPT_WAIT_VBLANK,
        OPT_BUFFER_COUNT,
    };
    static const struct option long_options[] = {
        { "no-cursor", no_argument, NULL, OPT_NO_CURSOR },
        { "color", required_argument, NULL, OPT_COLOR },
        { "yuv", required_argument, NULL, OPT_YUV },
        { "out-width", required_argument, NULL, OPT_OUT_WIDTH },
        { "out-height", required_argument, NULL, OPT_OUT_HEIGHT },
        { "h264-profile", required_argument, NULL, OPT_H264_PROFILE },
        { "h264-level", required_argument, NULL, OPT_H264_LEVEL },
        { "h264-cabac", required_argument, NULL, OPT_H264_CABAC },
        { "cpu-stage", no_argument, NULL, OPT_CPU_STAGE },
        { "xrgb-as-alpha", no_argument, NULL, OPT_XRGB_AS_ALPHA },
        { "wait-vblank", no_argument, NULL, OPT_WAIT_VBLANK },
        { "buffer-count", required_argument, NULL, OPT_BUFFER_COUNT },
        { "help", no_argument, NULL, 'h' },
        { 0, 0, 0, 0 },
    };

    while ((opt = getopt_long(argc, argv, "c:f:b:n:qm:y:Ch", long_options, NULL)) != -1) {
        switch (opt) {
        case 'c':
            card = optarg;
            break;
        case 'f':
            fps = atoi(optarg);
            break;
        case 'b':
            bitrate = atoi(optarg);
            break;
        case 'n':
            max_frames = atoi(optarg);
            break;
        case 'q':
            quiet = 1;
            break;
        case 'C':
        case OPT_NO_CURSOR:
            cursor_enabled = 0;
            break;
        case OPT_CPU_STAGE:
            cpu_stage = 1;
            break;
        case OPT_XRGB_AS_ALPHA:
            xrgb_as_alpha = 1;
            break;
        case OPT_WAIT_VBLANK:
            wait_vblank = 1;
            break;
        case OPT_BUFFER_COUNT:
            buffer_count = atoi(optarg);
            break;
        case 'm':
        case OPT_COLOR:
            if (strcmp(optarg, "bt709") == 0) {
                color_mode = IM_RGB_TO_YUV_BT709_LIMIT;
                color_name = "bt709";
            } else if (strcmp(optarg, "bt601") == 0) {
                color_mode = IM_RGB_TO_YUV_BT601_LIMIT;
                color_name = "bt601";
            } else if (strcmp(optarg, "default") == 0) {
                color_mode = IM_COLOR_SPACE_DEFAULT;
                color_name = "default";
            } else {
                fprintf(stderr, "invalid color mode: %s\n", optarg);
                usage(argv[0]);
                return 1;
            }
            break;
        case 'y':
        case OPT_YUV:
            if (strcmp(optarg, "nv12") == 0) {
                dst_rga_format = RK_FORMAT_YCbCr_420_SP;
                mpp_format = MPP_FMT_YUV420SP;
                yuv_name = "nv12";
            } else if (strcmp(optarg, "nv21") == 0) {
                dst_rga_format = RK_FORMAT_YCrCb_420_SP;
                mpp_format = MPP_FMT_YUV420SP_VU;
                yuv_name = "nv21";
            } else {
                fprintf(stderr, "invalid yuv mode: %s\n", optarg);
                usage(argv[0]);
                return 1;
            }
            break;
        case OPT_OUT_WIDTH:
            if (parse_u32_arg(optarg, &out_width) != 0) {
                fprintf(stderr, "invalid output width: %s\n", optarg);
                usage(argv[0]);
                return 1;
            }
            break;
        case OPT_OUT_HEIGHT:
            if (parse_u32_arg(optarg, &out_height) != 0) {
                fprintf(stderr, "invalid output height: %s\n", optarg);
                usage(argv[0]);
                return 1;
            }
            break;
        case OPT_H264_PROFILE:
            if (parse_h264_profile_arg(optarg, &h264_profile, &h264_profile_name) != 0) {
                fprintf(stderr, "invalid h264 profile: %s\n", optarg);
                usage(argv[0]);
                return 1;
            }
            break;
        case OPT_H264_LEVEL:
            h264_level = atoi(optarg);
            break;
        case OPT_H264_CABAC:
            h264_cabac = atoi(optarg) ? 1 : 0;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    if (fps <= 0 || fps > 120) {
        fprintf(stderr, "invalid fps: %d\n", fps);
        return 1;
    }
    if (bitrate < 100000 || bitrate > 200000000) {
        fprintf(stderr, "invalid bitrate: %d\n", bitrate);
        return 1;
    }
    if ((out_width && (out_width < 16 || out_width > 8192 || (out_width & 1))) ||
        (out_height && (out_height < 16 || out_height > 8192 || (out_height & 1)))) {
        fprintf(stderr, "output width/height must be even and between 16 and 8192\n");
        return 1;
    }
    if (h264_level < 10 || h264_level > 62) {
        fprintf(stderr, "invalid h264 level: %d\n", h264_level);
        return 1;
    }
    if (h264_profile == 66 && h264_cabac) {
        h264_cabac = 0;
    }
    if (buffer_count < 1 || buffer_count > 8) {
        fprintf(stderr, "buffer-count must be between 1 and 8\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, on_signal);
    signal(SIGUSR1, on_idr_signal);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* librga writes version/diagnostic text to stdout.  Preserve the original
     * stdout only for Annex-B and route library chatter to stderr. */
    int stream_fd = dup(STDOUT_FILENO);
    if (stream_fd < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        fprintf(stderr, "failed to isolate bitstream fd: %s\n", strerror(errno));
        if (stream_fd >= 0) close(stream_fd);
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);

    int drm_fd = open(card, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", card, strerror(errno));
        return 1;
    }
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);

    struct capture_fb cap;
    if (find_connected_display_fb(drm_fd, &cap) != 0) {
        close(drm_fd);
        return 1;
    }
    if (!out_width) out_width = cap.width;
    if (!out_height) out_height = cap.height;

    char fourcc[5];
    fourcc_to_string(cap.fourcc, fourcc);
    if (!quiet) {
        fprintf(stderr,
                "capture fb=%u crtc=%u %ux%u output=%ux%u fourcc=%s pitch=%u modifier=0x%016llx fps=%d bitrate=%d color=%s yuv=%s cursor=%s h264=%s/%d cabac=%d buffers=%d wait_vblank=%s\n",
                cap.fb_id, cap.crtc_id, cap.width, cap.height, out_width, out_height,
                fourcc, cap.pitch, (unsigned long long)cap.modifier,
                fps, bitrate, color_name, yuv_name,
                cursor_enabled ? "on" : "off",
                h264_profile_name, h264_level, h264_cabac,
                buffer_count, wait_vblank ? "on" : "off");
        if (xrgb_as_alpha) {
            fprintf(stderr, "xrgb-as-alpha=on: XRGB/XBGR scanout is passed to RGA as BGRA/RGBA without CPU copy\n");
        }
        if (cpu_stage) {
            fprintf(stderr, "cpu-stage=on: CPU copies scanout RGB into a RGA-compatible dumb buffer\n");
        }
    }

    MppCtx ctx = NULL;
    MppApi *mpi = NULL;
    MPP_RET ret = mpp_create(&ctx, &mpi);
    if (ret || !ctx || !mpi) {
        fprintf(stderr, "mpp_create failed %d\n", ret);
        close(drm_fd);
        return 1;
    }

    /* c145c84 MPP expects MppPollType here, and its sample configures this
     * before mpp_init().  Passing RK_S64 1000 after init can expose an
     * incomplete P-frame packet on this BSP. */
    MppPollType output_timeout = MPP_POLL_BLOCK;
    ret = mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &output_timeout);
    if (ret) {
        fprintf(stderr, "MPP_SET_OUTPUT_TIMEOUT failed %d\n", ret);
        mpp_destroy(ctx);
        close(drm_fd);
        return 1;
    }

    ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret) {
        fprintf(stderr, "mpp_init encoder failed %d\n", ret);
        mpp_destroy(ctx);
        close(drm_fd);
        return 1;
    }

    if (configure_encoder(ctx, mpi, out_width, out_height, fps, bitrate, mpp_format,
                          h264_profile, h264_level, h264_cabac) != 0) {
        mpp_destroy(ctx);
        close(drm_fd);
        return 1;
    }

    uint32_t out_ver_stride = (out_height + 15U) & ~15U;
    /* librga 2.2.0 on this BSP validates/accesses the imported destination
     * using the 4-byte XRGB footprint even though the operation outputs NV12.
     * Allocate for that vendor requirement plus a guard page region. */
    size_t rga_compat_size = (size_t)out_width * out_ver_stride * 4;
    size_t frame_alloc_size = rga_compat_size + 1U * 1024U * 1024U;
    MppBufferGroup group = NULL;
    ret = mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_DRM);
    if (ret) {
        fprintf(stderr, "mpp_buffer_group_get failed %d\n", ret);
        mpp_destroy(ctx);
        close(drm_fd);
        return 1;
    }

    struct encode_buffer *enc_buffers = calloc((size_t)buffer_count, sizeof(*enc_buffers));
    if (!enc_buffers) {
        fprintf(stderr, "alloc encode buffers failed\n");
        mpp_buffer_group_put(group);
        mpp_destroy(ctx);
        close(drm_fd);
        return 1;
    }
    if (init_encode_buffers(group, enc_buffers, buffer_count, frame_alloc_size,
                            out_width, out_height, dst_rga_format) != 0) {
        release_encode_buffers(enc_buffers, buffer_count);
        free(enc_buffers);
        mpp_buffer_group_put(group);
        mpp_destroy(ctx);
        close(drm_fd);
        return 1;
    }

    enum { PACKET_BUFFER_COUNT = 8 };
    MppBufferGroup packet_group = NULL;
    MppBuffer packet_buffers[PACKET_BUFFER_COUNT] = { 0 };
    size_t packet_alloc_size = (size_t)out_width * out_ver_stride * 3 / 2 +
                               1U * 1024U * 1024U;
    ret = mpp_buffer_group_get_internal(&packet_group, MPP_BUFFER_TYPE_DRM);
    if (ret) {
        fprintf(stderr, "mpp packet buffer group failed %d\n", ret);
        release_encode_buffers(enc_buffers, buffer_count);
        free(enc_buffers);
        mpp_buffer_group_put(group);
        mpp_destroy(ctx);
        close(drm_fd);
        return 1;
    }
    for (int i = 0; i < PACKET_BUFFER_COUNT; i++) {
        ret = mpp_buffer_get(packet_group, &packet_buffers[i], packet_alloc_size);
        if (ret || !packet_buffers[i]) {
            fprintf(stderr, "mpp packet buffer allocation failed %d\n", ret);
            for (int j = 0; j < PACKET_BUFFER_COUNT; j++) {
                if (packet_buffers[j]) mpp_buffer_put(packet_buffers[j]);
            }
            mpp_buffer_group_put(packet_group);
            release_encode_buffers(enc_buffers, buffer_count);
            free(enc_buffers);
            mpp_buffer_group_put(group);
            mpp_destroy(ctx);
            close(drm_fd);
            return 1;
        }
    }
    int rgb_format = rga_format_for_drm(cap.fourcc, xrgb_as_alpha);
    int rgb_bytes_per_pixel = bytes_per_pixel_for_drm(cap.fourcc);
    int stage_rgb_format = rgb_format;
    if (cpu_stage && rgb_format == RK_FORMAT_BGRX_8888) {
        stage_rgb_format = RK_FORMAT_BGRA_8888;
    }
    struct dumb_rgb_buffer rgb_stage;
    memset(&rgb_stage, 0, sizeof(rgb_stage));
    rgb_stage.dma_fd = -1;

    if (cursor_enabled || cpu_stage) {
        if (!rgb_format || !rgb_bytes_per_pixel) {
            fprintf(stderr, "RGB stage needs a source format supported by RGA\n");
            release_encode_buffers(enc_buffers, buffer_count);
            free(enc_buffers);
            mpp_buffer_group_put(group);
            mpp_destroy(ctx);
            close(drm_fd);
            return 1;
        }

        if (create_dumb_rgb_buffer(drm_fd, &rgb_stage, cap.width, cap.height,
                                   stage_rgb_format, rgb_bytes_per_pixel) != 0) {
            release_encode_buffers(enc_buffers, buffer_count);
            free(enc_buffers);
            mpp_buffer_group_put(group);
            mpp_destroy(ctx);
            close(drm_fd);
            return 1;
        }
    }

    struct src_import src;
    memset(&src, 0, sizeof(src));
    src.dma_fd = -1;

    struct cursor_import cursor;
    memset(&cursor, 0, sizeof(cursor));
    cursor.dma_fd = -1;

    struct timespec next_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);
    long frame_interval_ns = 1000000000L / fps;
    int64_t frame_index = 0;
    int exit_code = 0;

    while (!stop_requested) {
        if (wait_vblank) {
            static int warned_vblank;
            if (wait_for_crtc_vblank(drm_fd, cap.crtc_id) != 0 && !warned_vblank) {
                fprintf(stderr, "drmWaitVBlank failed: %s; continuing without vblank sync\n", strerror(errno));
                warned_vblank = 1;
            }
        }

        struct capture_fb cur;
        if (find_connected_display_fb(drm_fd, &cur) != 0) {
            exit_code = 1;
            break;
        }
        if (cur.width != cap.width || cur.height != cap.height || cur.fourcc != cap.fourcc) {
            char old_fourcc[5];
            char new_fourcc[5];
            fourcc_to_string(cap.fourcc, old_fourcc);
            fourcc_to_string(cur.fourcc, new_fourcc);
            fprintf(stderr, "mode changed from %ux%u %s to %ux%u %s; restart needed\n",
                    cap.width, cap.height, old_fourcc,
                    cur.width, cur.height, new_fourcc);
            exit_code = 1;
            break;
        }

        if (ensure_src_import(drm_fd, &src, &cur, cpu_stage, xrgb_as_alpha) != 0) {
            exit_code = 1;
            break;
        }
        warn_active_overlay_planes(drm_fd, cap.crtc_id, cur.fb_id);

        struct encode_buffer *enc = &enc_buffers[frame_index % buffer_count];
        rga_buffer_t dst = enc->rga_buffer;
        int test_solid = getenv("REMYDESK_TEST_SOLID_NV12") != NULL;

        if (!test_solid) {
          IM_STATUS status;
          if (cursor_enabled || cpu_stage) {
            if (cpu_stage) {
                if (copy_fb_to_rgb_stage(&src, &rgb_stage, cap.width, cap.height,
                                         rgb_bytes_per_pixel) != 0) {
                    exit_code = 1;
                    break;
                }
            } else {
                status = imcopy(src.rga_buffer, rgb_stage.rga_buffer);
                if (status != IM_STATUS_SUCCESS) {
                    fprintf(stderr, "RGA RGB copy failed: %s\n", imStrError(status));
                    exit_code = 1;
                    break;
                }
            }

            if (cursor_enabled) {
                struct cursor_state cursor_state;
                if (find_cursor_plane(drm_fd, cap.crtc_id, &cursor_state) != 0) {
                    fprintf(stderr, "DRM cursor plane query failed\n");
                    exit_code = 1;
                    break;
                }
                if (ensure_cursor_import(drm_fd, &cursor, &cursor_state) != 0) {
                    exit_code = 1;
                    break;
                }
                if (blend_cursor_with_rga(&cursor, &cursor_state,
                                          rgb_stage.rga_buffer, cap.width, cap.height) != 0) {
                    exit_code = 1;
                    break;
                }
            }

            rga_buffer_t color_src = rgb_stage.rga_buffer;
            color_src.color_space_mode = color_mode;
            dst.color_space_mode = color_mode;
            if (rga_convert_frame(color_src, dst, cap.width, cap.height,
                                  out_width, out_height,
                                  "RGA RGB->YUV conversion") != 0) {
                exit_code = 1;
                break;
            }
          } else {
            rga_buffer_t color_src = src.rga_buffer;
            color_src.color_space_mode = color_mode;
            dst.color_space_mode = color_mode;
            if (rga_convert_frame(color_src, dst, cap.width, cap.height,
                                  out_width, out_height,
                                  "RGA RGB->YUV conversion") != 0) {
                exit_code = 1;
                break;
            }
          }
        }

        if (test_solid) {
            uint8_t *ptr = mpp_buffer_get_ptr(enc->mpp_buf);
            if (!ptr) {
                fprintf(stderr, "test NV12 mapping failed\n");
                exit_code = 1;
                break;
            }
            memset(ptr, 16, (size_t)out_width * out_ver_stride);
            memset(ptr + (size_t)out_width * out_ver_stride, 128,
                   (size_t)out_width * out_ver_stride / 2);
        }

        /* RGA and MPP are separate DMA masters.  IM_SYNC waits for the RGA
         * job, but this older BSP also needs an explicit dma-buf ownership /
         * cache transition before the encoder consumes the NV12 surface. */
        if (dma_buf_publish(enc->dma_fd) != 0 && getenv("REMYDESK_PACKET_DEBUG")) {
            fprintf(stderr, "DMA_BUF_IOCTL_SYNC failed: %s\n", strerror(errno));
        }

        if (frame_index == 0) {
            force_idr_requested = 1;
        }

        int64_t pts_us = frame_index * 1000000LL / fps;
        MppBuffer pkt_buf = packet_buffers[frame_index % PACKET_BUFFER_COUNT];
        if (encode_one_frame(ctx, mpi, enc->mpp_buf, pkt_buf,
                             out_width, out_height,
                             mpp_format, pts_us, stream_fd) != 0) {
            if (!stop_requested) {
                exit_code = 1;
            }
            break;
        }

        frame_index++;
        if (max_frames > 0 && frame_index >= max_frames) {
            break;
        }

        add_ns(&next_frame, frame_interval_ns);
        while (!stop_requested &&
               clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL) == EINTR) {
        }
    }

    release_cursor_import(&cursor);
    release_src_import(&src);
    if (cursor_enabled || cpu_stage) {
        release_dumb_rgb_buffer(drm_fd, &rgb_stage);
    }
    release_encode_buffers(enc_buffers, buffer_count);
    free(enc_buffers);
    for (int i = 0; i < PACKET_BUFFER_COUNT; i++) {
        if (packet_buffers[i]) mpp_buffer_put(packet_buffers[i]);
    }
    mpp_buffer_group_put(packet_group);
    mpp_buffer_group_put(group);
    mpp_destroy(ctx);
    close(drm_fd);
    close(stream_fd);

    if (!quiet) {
        fprintf(stderr, "stopped after %lld frames\n", (long long)frame_index);
    }
    return exit_code;
}

