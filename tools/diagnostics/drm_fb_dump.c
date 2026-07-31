#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int write_all(int fd, const void *data, size_t size)
{
    const uint8_t *cursor = data;

    while (size) {
        ssize_t written = write(fd, cursor, size);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        cursor += written;
        size -= (size_t)written;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s /dev/dri/card0 <fb-id> <output.ppm>\n", argv[0]);
        return 2;
    }

    char *end = NULL;
    unsigned long requested_id = strtoul(argv[2], &end, 10);
    if (!end || *end || !requested_id || requested_id > UINT32_MAX) {
        fprintf(stderr, "invalid framebuffer id: %s\n", argv[2]);
        return 2;
    }

    int drm_fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    drmModeFB2Ptr fb = drmModeGetFB2(drm_fd, (uint32_t)requested_id);
    if (!fb) {
        fprintf(stderr, "drmModeGetFB2(%lu): %s\n", requested_id, strerror(errno));
        close(drm_fd);
        return 1;
    }

    if (fb->pixel_format != DRM_FORMAT_ARGB8888 && fb->pixel_format != DRM_FORMAT_XRGB8888) {
        fprintf(stderr, "unsupported format 0x%08x\n", fb->pixel_format);
        drmModeFreeFB2(fb);
        close(drm_fd);
        return 1;
    }

    struct drm_mode_map_dumb map_request = { 0 };
    map_request.handle = fb->handles[0];
    if (!map_request.handle || ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_request) < 0) {
        fprintf(stderr, "DRM_IOCTL_MODE_MAP_DUMB(handle=%u): %s\n",
            map_request.handle, strerror(errno));
        drmModeFreeFB2(fb);
        close(drm_fd);
        return 1;
    }

    size_t map_size = (size_t)fb->offsets[0] + (size_t)fb->pitches[0] * fb->height;
    uint8_t *mapped = mmap(NULL, map_size, PROT_READ, MAP_SHARED, drm_fd, map_request.offset);
    if (mapped == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        drmModeFreeFB2(fb);
        close(drm_fd);
        return 1;
    }

    int output_fd = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (output_fd < 0) {
        fprintf(stderr, "open %s: %s\n", argv[3], strerror(errno));
        munmap(mapped, map_size);
        drmModeFreeFB2(fb);
        close(drm_fd);
        return 1;
    }

    char header[64];
    int header_size = snprintf(header, sizeof(header), "P6\n%u %u\n255\n", fb->width, fb->height);
    if (write_all(output_fd, header, (size_t)header_size) < 0) {
        fprintf(stderr, "write header: %s\n", strerror(errno));
        close(output_fd);
        munmap(mapped, map_size);
        drmModeFreeFB2(fb);
        close(drm_fd);
        return 1;
    }

    uint8_t *rgb = malloc((size_t)fb->width * 3);
    if (!rgb) {
        fprintf(stderr, "malloc row failed\n");
        close(output_fd);
        munmap(mapped, map_size);
        drmModeFreeFB2(fb);
        close(drm_fd);
        return 1;
    }

    const uint8_t *pixels = mapped + fb->offsets[0];
    for (uint32_t y = 0; y < fb->height; ++y) {
        const uint8_t *source = pixels + (size_t)y * fb->pitches[0];
        for (uint32_t x = 0; x < fb->width; ++x) {
            rgb[x * 3] = source[x * 4 + 2];
            rgb[x * 3 + 1] = source[x * 4 + 1];
            rgb[x * 3 + 2] = source[x * 4];
        }
        if (write_all(output_fd, rgb, (size_t)fb->width * 3) < 0) {
            fprintf(stderr, "write row: %s\n", strerror(errno));
            free(rgb);
            close(output_fd);
            munmap(mapped, map_size);
            drmModeFreeFB2(fb);
            close(drm_fd);
            return 1;
        }
    }

    fprintf(stderr, "dumped fb=%lu format=0x%08x size=%ux%u pitch=%u\n",
        requested_id, fb->pixel_format, fb->width, fb->height, fb->pitches[0]);

    free(rgb);
    close(output_fd);
    munmap(mapped, map_size);
    drmModeFreeFB2(fb);
    close(drm_fd);
    return 0;
}
