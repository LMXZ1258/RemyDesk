#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rga/im2d.h>
#include <rga/im2d_mpi.h>
#include <rga/rga.h>

int main(void)
{
    const int width = 1920;
    const int height = 1080;
    size_t src_size = (size_t)width * height * 4;
    size_t dst_size = (size_t)width * height * 3 / 2;

    void *src_mem = NULL;
    void *dst_mem = NULL;
    if (posix_memalign(&src_mem, 64, src_size) != 0 ||
        posix_memalign(&dst_mem, 64, dst_size) != 0) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }
    memset(src_mem, 0x80, src_size);
    memset(dst_mem, 0, dst_size);

    im_handle_param_t dst_param = {
        .width = width,
        .height = height,
        .format = RK_FORMAT_YCbCr_420_SP,
    };

    struct {
        const char *name;
        int format;
        int bpp;
    } formats[] = {
        { "BGRX", RK_FORMAT_BGRX_8888, 4 },
        { "RGBX", RK_FORMAT_RGBX_8888, 4 },
        { "BGRA", RK_FORMAT_BGRA_8888, 4 },
        { "RGBA", RK_FORMAT_RGBA_8888, 4 },
        { "BGR888", RK_FORMAT_BGR_888, 3 },
        { "RGB888", RK_FORMAT_RGB_888, 3 },
    };

    int cores[] = {
        IM_SCHEDULER_RGA2_CORE0,
        IM_SCHEDULER_RGA3_CORE0,
        IM_SCHEDULER_RGA3_CORE1,
    };

    for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
        im_handle_param_t src_param = {
            .width = width,
            .height = height,
            .format = formats[f].format,
        };

        rga_buffer_handle_t src_handle = importbuffer_virtualaddr(src_mem, &src_param);
        rga_buffer_handle_t dst_handle = importbuffer_virtualaddr(dst_mem, &dst_param);
        if (!src_handle || !dst_handle) {
            fprintf(stderr, "import failed format=%s src=%u dst=%u\n",
                    formats[f].name, src_handle, dst_handle);
            if (src_handle) releasebuffer_handle(src_handle);
            if (dst_handle) releasebuffer_handle(dst_handle);
            continue;
        }

        rga_buffer_t src = wrapbuffer_handle(src_handle, width, height, formats[f].format,
                                             width, height);
        rga_buffer_t dst = wrapbuffer_handle(dst_handle, width, height, RK_FORMAT_YCbCr_420_SP,
                                             width, height);
        im_rect rect = { 0, 0, width, height };
        im_rect empty_rect = { 0, 0, 0, 0 };
        rga_buffer_t empty = { 0 };

        IM_STATUS cvt = imcvtcolor(src, dst, formats[f].format,
                                   RK_FORMAT_YCbCr_420_SP,
                                   IM_RGB_TO_YUV_BT601_LIMIT, 1);
        printf("format=%s imcvtcolor status=%d %s\n",
               formats[f].name, cvt, imStrError(cvt));

        for (size_t i = 0; i < sizeof(cores) / sizeof(cores[0]); i++) {
            im_opt_t opt;
            memset(&opt, 0, sizeof(opt));
            opt.version = RGA_CURRENT_API_HEADER_VERSION;
            opt.core = cores[i];
            IM_STATUS status = improcess_ctx(src, dst, empty, rect, rect, empty_rect,
                                             -1, NULL, &opt, IM_SYNC, 0);
            printf("format=%s core=0x%x status=%d %s\n",
                   formats[f].name, cores[i], status, imStrError(status));
        }

        releasebuffer_handle(src_handle);
        releasebuffer_handle(dst_handle);
    }

    free(src_mem);
    free(dst_mem);
    return 0;
}
