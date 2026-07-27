#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif

static const char *connector_type_name(uint32_t type)
{
    switch (type) {
    case DRM_MODE_CONNECTOR_VGA: return "VGA";
    case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
    case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
    case DRM_MODE_CONNECTOR_Composite: return "Composite";
    case DRM_MODE_CONNECTOR_SVIDEO: return "SVIDEO";
    case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
    case DRM_MODE_CONNECTOR_Component: return "Component";
    case DRM_MODE_CONNECTOR_9PinDIN: return "DIN";
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV: return "TV";
    case DRM_MODE_CONNECTOR_eDP: return "eDP";
    case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
    case DRM_MODE_CONNECTOR_DSI: return "DSI";
    case DRM_MODE_CONNECTOR_DPI: return "DPI";
    default: return "Unknown";
    }
}

static const char *connection_name(uint32_t connection)
{
    switch (connection) {
    case DRM_MODE_CONNECTED: return "connected";
    case DRM_MODE_DISCONNECTED: return "disconnected";
    case DRM_MODE_UNKNOWNCONNECTION: return "unknown";
    default: return "invalid";
    }
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

static void print_fb_export_attempt(int fd, uint32_t fb_id)
{
    drmModeFB2Ptr fb = drmModeGetFB2(fd, fb_id);
    if (!fb) {
        printf("      drmModeGetFB2(%u) failed: %s\n", fb_id, strerror(errno));
        return;
    }

    char fourcc[5];
    fourcc_to_string(fb->pixel_format, fourcc);
    printf("      fb %u: %ux%u fourcc=%s flags=0x%x\n",
           fb->fb_id, fb->width, fb->height, fourcc, fb->flags);

    for (int i = 0; i < 4; i++) {
        if (!fb->handles[i]) {
            continue;
        }

        int duplicate = 0;
        for (int j = 0; j < i; j++) {
            if (fb->handles[j] == fb->handles[i]) {
                duplicate = 1;
            }
        }
        if (duplicate) {
            printf("        plane[%d]: handle=%u pitch=%u offset=%u modifier=0x%llx duplicate\n",
                   i, fb->handles[i], fb->pitches[i], fb->offsets[i],
                   (unsigned long long)fb->modifier);
            continue;
        }

        int prime_fd = -1;
        int rc = drmPrimeHandleToFD(fd, fb->handles[i], DRM_CLOEXEC, &prime_fd);
        if (rc == 0) {
            printf("        plane[%d]: handle=%u pitch=%u offset=%u modifier=0x%llx -> dmabuf_fd=%d export=ok\n",
                   i, fb->handles[i], fb->pitches[i], fb->offsets[i],
                   (unsigned long long)fb->modifier, prime_fd);
            close(prime_fd);
        } else {
            printf("        plane[%d]: handle=%u pitch=%u offset=%u modifier=0x%llx export=failed errno=%d %s\n",
                   i, fb->handles[i], fb->pitches[i], fb->offsets[i],
                   (unsigned long long)fb->modifier, errno, strerror(errno));
        }
    }

    drmModeFreeFB2(fb);
}

static int crtc_index_for_id(drmModeRes *res, uint32_t crtc_id)
{
    for (int i = 0; i < res->count_crtcs; i++) {
        if ((uint32_t)res->crtcs[i] == crtc_id) {
            return i;
        }
    }
    return -1;
}

static void inspect_card(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("== %s ==\nopen failed: %s\n", path, strerror(errno));
        return;
    }

    printf("== %s ==\n", path);
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        printf("drmModeGetResources failed: %s\n", strerror(errno));
        close(fd);
        return;
    }

    printf("connectors=%d crtcs=%d encoders=%d\n",
           res->count_connectors, res->count_crtcs, res->count_encoders);

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) {
            continue;
        }

        drmModeEncoder *enc = find_encoder_for_connector(fd, conn);
        uint32_t crtc_id = enc ? enc->crtc_id : 0;
        printf("  connector %u %s-%u %s modes=%d encoder=%u crtc=%u\n",
               conn->connector_id,
               connector_type_name(conn->connector_type),
               conn->connector_type_id,
               connection_name(conn->connection),
               conn->count_modes,
               enc ? enc->encoder_id : 0,
               crtc_id);

        if (crtc_id) {
            drmModeCrtc *crtc = drmModeGetCrtc(fd, crtc_id);
            if (crtc) {
                printf("    crtc %u: fb=%u pos=%d,%d size=%ux%u mode_valid=%d\n",
                       crtc->crtc_id, crtc->buffer_id, crtc->x, crtc->y,
                       crtc->width, crtc->height, crtc->mode_valid);
                if (crtc->buffer_id) {
                    print_fb_export_attempt(fd, crtc->buffer_id);
                }
                drmModeFreeCrtc(crtc);
            }
        }

        if (enc) {
            drmModeFreeEncoder(enc);
        }
        drmModeFreeConnector(conn);
    }

    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (!planes) {
        printf("drmModeGetPlaneResources failed: %s\n", strerror(errno));
        drmModeFreeResources(res);
        close(fd);
        return;
    }

    printf("planes=%u\n", planes->count_planes);
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(fd, planes->planes[i]);
        if (!plane) {
            continue;
        }

        int crtc_index = crtc_index_for_id(res, plane->crtc_id);
        printf("  plane %u: crtc=%u(index=%d) fb=%u possible_crtcs=0x%x formats=%u\n",
               plane->plane_id, plane->crtc_id, crtc_index, plane->fb_id,
               plane->possible_crtcs, plane->count_formats);
        if (plane->fb_id) {
            print_fb_export_attempt(fd, plane->fb_id);
        }
        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(planes);
    drmModeFreeResources(res);
    close(fd);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            inspect_card(argv[i]);
        }
        return 0;
    }

    inspect_card("/dev/dri/card0");
    inspect_card("/dev/dri/card1");
    return 0;
}
