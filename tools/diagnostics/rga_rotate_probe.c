/*
 * Validate Rockchip RGA rotation between two PRIME-exported DRM dumb buffers.
 * The minimal IM2D ABI declarations match Rockchip librga's Apache-2.0 headers.
 */

#include <dlfcn.h>
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

#define RK_FORMAT_RGBA_8888 (0x00 << 8)
#define RK_FORMAT_RGBX_8888 (0x01 << 8)
#define RK_FORMAT_BGRA_8888 (0x03 << 8)
#define RK_FORMAT_BGRX_8888 (0x16 << 8)
#define RK_FORMAT_ARGB_8888 (0x28 << 8)
#define RK_FORMAT_XRGB_8888 (0x29 << 8)
#define RK_FORMAT_ABGR_8888 (0x2c << 8)
#define RK_FORMAT_XBGR_8888 (0x2d << 8)
#define IM_HAL_TRANSFORM_ROT_90 (1 << 0)

typedef uint32_t rga_buffer_handle_t;

typedef struct {
    int max;
    int min;
} im_colorkey_range;

typedef struct {
    int scale_r;
    int scale_g;
    int scale_b;
    int offset_r;
    int offset_g;
    int offset_b;
} im_nn_t;

typedef struct {
    void *vir_addr;
    void *phy_addr;
    int fd;
    int width;
    int height;
    int wstride;
    int hstride;
    int format;
    int color_space_mode;
    union {
        int global_alpha;
        struct {
            uint16_t alpha0;
            uint16_t alpha1;
        } alpha_bit;
    };
    int rd_mode;
    int color;
    im_colorkey_range colorkey_range;
    im_nn_t nn;
    int rop_code;
    rga_buffer_handle_t handle;
} rga_buffer_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t format;
} im_handle_param_t;

typedef rga_buffer_handle_t (*importbuffer_fd_fn)(int, im_handle_param_t *);
typedef rga_buffer_t (*wrapbuffer_handle_fn)(rga_buffer_handle_t, int, int, int, int, int);
typedef int (*releasebuffer_handle_fn)(rga_buffer_handle_t);
typedef int (*imrotate_fn)(const rga_buffer_t, rga_buffer_t, int, int);

struct dumb_buffer {
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    int prime_fd;
    uint8_t *mapping;
};

static int create_dumb(int drm_fd, uint32_t width, uint32_t height, struct dumb_buffer *buffer)
{
    struct drm_mode_create_dumb create = { 0 };
    struct drm_mode_map_dumb map = { 0 };

    create.width = width;
    create.height = height;
    create.bpp = 32;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
        return -1;

    buffer->handle = create.handle;
    buffer->pitch = create.pitch;
    buffer->size = create.size;
    buffer->width = width;
    buffer->height = height;
    buffer->prime_fd = -1;

    map.handle = create.handle;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
        return -1;

    buffer->mapping = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE,
        MAP_SHARED, drm_fd, (off_t)map.offset);
    if (buffer->mapping == MAP_FAILED) {
        buffer->mapping = NULL;
        return -1;
    }

    struct drm_prime_handle prime = { 0 };
    prime.handle = buffer->handle;
    prime.flags = DRM_CLOEXEC | DRM_RDWR;
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0)
        return -1;
    buffer->prime_fd = prime.fd;
    return 0;
}

static void destroy_dumb(int drm_fd, struct dumb_buffer *buffer)
{
    if (buffer->prime_fd >= 0)
        close(buffer->prime_fd);
    if (buffer->mapping)
        munmap(buffer->mapping, buffer->size);
    if (buffer->handle) {
        struct drm_mode_destroy_dumb destroy = { 0 };
        destroy.handle = buffer->handle;
        ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
}

static void fill_test_pattern(struct dumb_buffer *buffer)
{
    static const uint32_t colors[4] = {
        0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff
    };
    for (uint32_t y = 0; y < buffer->height; ++y) {
        uint32_t *row = (uint32_t *)(buffer->mapping + (size_t)y * buffer->pitch);
        for (uint32_t x = 0; x < buffer->width; ++x)
            row[x] = colors[(y >= buffer->height / 2) * 2 + (x >= buffer->width / 2)];
    }
}

static int is_test_color(uint32_t pixel)
{
    pixel |= 0xff000000;
    return pixel == 0xffff0000 || pixel == 0xff00ff00
        || pixel == 0xff0000ff || pixel == 0xffffffff;
}

static int parse_rga_format(const char *name)
{
    static const struct {
        const char *name;
        int format;
    } formats[] = {
        { "rgba", RK_FORMAT_RGBA_8888 },
        { "rgbx", RK_FORMAT_RGBX_8888 },
        { "bgra", RK_FORMAT_BGRA_8888 },
        { "bgrx", RK_FORMAT_BGRX_8888 },
        { "argb", RK_FORMAT_ARGB_8888 },
        { "xrgb", RK_FORMAT_XRGB_8888 },
        { "abgr", RK_FORMAT_ABGR_8888 },
        { "xbgr", RK_FORMAT_XBGR_8888 },
    };

    for (size_t index = 0; index < sizeof(formats) / sizeof(formats[0]); ++index) {
        if (!strcmp(name, formats[index].name))
            return formats[index].format;
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *drm_path = argc > 1 ? argv[1] : "/dev/dri/card0";
    const char *rga_path = argc > 2 ? argv[2] : "librga.so.2";
    const char *source_format_name = argc > 3 ? argv[3] : "bgrx";
    const char *destination_format_name = argc > 4 ? argv[4] : "bgra";
    int source_format = parse_rga_format(source_format_name);
    int destination_format = parse_rga_format(destination_format_name);
    struct dumb_buffer source = { .prime_fd = -1 };
    struct dumb_buffer destination = { .prime_fd = -1 };
    rga_buffer_handle_t source_handle = 0;
    rga_buffer_handle_t destination_handle = 0;
    int result = 1;

    if (source_format < 0 || destination_format < 0) {
        fprintf(stderr, "unknown format; use rgba/rgbx/bgra/bgrx/argb/xrgb/abgr/xbgr\n");
        return 2;
    }

    int drm_fd = open(drm_path, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "open %s: %s\n", drm_path, strerror(errno));
        return 1;
    }

    void *library = dlopen(rga_path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "dlopen %s: %s\n", rga_path, dlerror());
        close(drm_fd);
        return 1;
    }

    importbuffer_fd_fn importbuffer_fd = (importbuffer_fd_fn)dlsym(library, "importbuffer_fd");
    wrapbuffer_handle_fn wrapbuffer_handle_t = (wrapbuffer_handle_fn)dlsym(library, "wrapbuffer_handle_t");
    releasebuffer_handle_fn releasebuffer_handle = (releasebuffer_handle_fn)dlsym(library, "releasebuffer_handle");
    imrotate_fn imrotate_t = (imrotate_fn)dlsym(library, "imrotate_t");
    if (!importbuffer_fd || !wrapbuffer_handle_t || !releasebuffer_handle || !imrotate_t) {
        fprintf(stderr, "librga is missing required IM2D symbols\n");
        goto out;
    }

    if (create_dumb(drm_fd, 64, 32, &source) < 0
        || create_dumb(drm_fd, 32, 64, &destination) < 0) {
        fprintf(stderr, "create/export dumb buffer: %s\n", strerror(errno));
        goto out;
    }
    fill_test_pattern(&source);
    memset(destination.mapping, 0, destination.size);

    im_handle_param_t source_param = {
        .width = source.pitch / 4,
        .height = source.height,
        .format = (uint32_t)source_format,
    };
    im_handle_param_t destination_param = {
        .width = destination.pitch / 4,
        .height = destination.height,
        .format = (uint32_t)destination_format,
    };
    source_handle = importbuffer_fd(source.prime_fd, &source_param);
    destination_handle = importbuffer_fd(destination.prime_fd, &destination_param);
    if (!source_handle || !destination_handle) {
        fprintf(stderr, "RGA import failed source=%u destination=%u\n",
            source_handle, destination_handle);
        goto out;
    }

    rga_buffer_t source_rga = wrapbuffer_handle_t(source_handle,
        source.width, source.height, source.pitch / 4, source.height, source_format);
    rga_buffer_t destination_rga = wrapbuffer_handle_t(destination_handle,
        destination.width, destination.height, destination.pitch / 4, destination.height,
        destination_format);
    int rotate_result = imrotate_t(source_rga, destination_rga,
        IM_HAL_TRANSFORM_ROT_90, 1);
    if (rotate_result <= 0) {
        fprintf(stderr, "RGA rotate failed: %d\n", rotate_result);
        goto out;
    }

    uint32_t corners[4] = {
        *(uint32_t *)(destination.mapping + destination.pitch + 4),
        *(uint32_t *)(destination.mapping + destination.pitch + (destination.width - 2) * 4),
        *(uint32_t *)(destination.mapping + (destination.height - 2) * destination.pitch + 4),
        *(uint32_t *)(destination.mapping + (destination.height - 2) * destination.pitch
            + (destination.width - 2) * 4),
    };
    if (!is_test_color(corners[0]) || !is_test_color(corners[1])
        || !is_test_color(corners[2]) || !is_test_color(corners[3])) {
        fprintf(stderr, "RGA output verification failed: %08x %08x %08x %08x\n",
            corners[0], corners[1], corners[2], corners[3]);
        goto out;
    }

    printf("RGA_ROTATE_OK formats=%s->%s colors=%08x,%08x,%08x,%08x source=%ux%u pitch=%u destination=%ux%u pitch=%u result=%d\n",
        source_format_name, destination_format_name,
        corners[0], corners[1], corners[2], corners[3],
        source.width, source.height, source.pitch, destination.width,
        destination.height, destination.pitch, rotate_result);
    result = 0;

out:
    if (destination_handle)
        releasebuffer_handle(destination_handle);
    if (source_handle)
        releasebuffer_handle(source_handle);
    destroy_dumb(drm_fd, &destination);
    destroy_dumb(drm_fd, &source);
    dlclose(library);
    close(drm_fd);
    return result;
}
