#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

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

int main(int argc, char **argv)
{
    const char *card = argc > 1 ? argv[1] : "/dev/dri/card0";
    int fd = open(card, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", card, strerror(errno));
        return 1;
    }

    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        fprintf(stderr, "drmModeGetResources failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    uint32_t crtc_id = 0;
    for (int i = 0; i < res->count_connectors && !crtc_id; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) {
            continue;
        }
        if ((conn->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
             conn->connector_type == DRM_MODE_CONNECTOR_HDMIB) &&
            conn->connection == DRM_MODE_CONNECTED) {
            drmModeEncoder *enc = find_encoder_for_connector(fd, conn);
            if (enc) {
                crtc_id = enc->crtc_id;
                drmModeFreeEncoder(enc);
            }
        }
        drmModeFreeConnector(conn);
    }

    drmModeCrtc *crtc = crtc_id ? drmModeGetCrtc(fd, crtc_id) : NULL;
    drmModeFB2Ptr fb = crtc && crtc->buffer_id ? drmModeGetFB2(fd, crtc->buffer_id) : NULL;
    if (!fb || !fb->handles[0]) {
        fprintf(stderr, "no active framebuffer handle\n");
        if (fb) drmModeFreeFB2(fb);
        if (crtc) drmModeFreeCrtc(crtc);
        drmModeFreeResources(res);
        close(fd);
        return 1;
    }

    int dma_fd = -1;
    if (drmPrimeHandleToFD(fd, fb->handles[0], DRM_CLOEXEC | DRM_RDWR, &dma_fd) != 0) {
        fprintf(stderr, "drmPrimeHandleToFD failed: %s\n", strerror(errno));
        drmModeFreeFB2(fb);
        drmModeFreeCrtc(crtc);
        drmModeFreeResources(res);
        close(fd);
        return 1;
    }

    size_t size = (size_t)fb->pitches[0] * fb->height;
    void *ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, dma_fd, 0);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "mmap dmabuf failed: %s\n", strerror(errno));
        close(dma_fd);
        drmModeFreeFB2(fb);
        drmModeFreeCrtc(crtc);
        drmModeFreeResources(res);
        close(fd);
        return 1;
    }

    volatile uint8_t first = ((volatile uint8_t *)ptr)[0];
    printf("mmap ok fb=%u %ux%u pitch=%u size=%zu first=%u\n",
           fb->fb_id, fb->width, fb->height, fb->pitches[0], size, first);

    munmap(ptr, size);
    close(dma_fd);
    drmModeFreeFB2(fb);
    drmModeFreeCrtc(crtc);
    drmModeFreeResources(res);
    close(fd);
    return 0;
}
