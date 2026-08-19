/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <drm.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef GBM_BO_USE_LINEAR
#define GBM_BO_USE_LINEAR (1U << 4)
#endif

typedef struct gbm_bo *(*GbmCreateWithModifiers2Fn)(
    struct gbm_device *, uint32_t, uint32_t, uint32_t,
    const uint64_t *, unsigned int, uint32_t);
typedef int (*GbmGetFdForPlaneFn)(struct gbm_bo *, int);

static bool contains_case_insensitive(const char *value, const char *needle)
{
    if (!value || !needle)
        return false;
    size_t needle_length = strlen(needle);
    for (const char *cursor = value; *cursor; ++cursor) {
        if (!strncasecmp(cursor, needle, needle_length))
            return true;
    }
    return false;
}

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint compiled = GL_FALSE;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        char log[512] = { 0 };
        glGetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "gpu_probe: shader compile failed: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool test_gles(void)
{
    static const char vertex_source[] =
        "attribute vec2 position;"
        "void main(){gl_Position=vec4(position,0.0,1.0);}";
    static const char fragment_source[] =
        "precision mediump float;"
        "void main(){gl_FragColor=vec4(0.125,0.5,0.875,1.0);}";
    static const GLfloat vertices[] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment)
        return false;

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        fprintf(stderr, "gpu_probe: shader link failed\n");
        glDeleteProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return false;
    }

    glViewport(0, 0, 8, 8);
    glUseProgram(program);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char pixel[4] = { 0 };
    glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    GLenum error = glGetError();
    glDeleteProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    if (error != GL_NO_ERROR || pixel[3] < 240) {
        fprintf(stderr,
                "gpu_probe: GLES readback failed error=0x%x rgba=%u,%u,%u,%u\n",
                error, pixel[0], pixel[1], pixel[2], pixel[3]);
        return false;
    }
    return true;
}

static bool test_linear_scanout(struct gbm_device *gbm, int drm_fd)
{
    const uint32_t width = 64;
    const uint32_t height = 64;
    const uint64_t linear_modifier = DRM_FORMAT_MOD_LINEAR;
    GbmCreateWithModifiers2Fn create_with_modifiers2 =
        (GbmCreateWithModifiers2Fn)dlsym(RTLD_DEFAULT,
                                         "gbm_bo_create_with_modifiers2");
    GbmGetFdForPlaneFn get_fd_for_plane =
        (GbmGetFdForPlaneFn)dlsym(RTLD_DEFAULT, "gbm_bo_get_fd_for_plane");
    if (!create_with_modifiers2 || !get_fd_for_plane) {
        fprintf(stderr, "gpu_probe: required GBM compatibility symbols missing\n");
        return false;
    }

    struct gbm_bo *bo = create_with_modifiers2(
        gbm, width, height, DRM_FORMAT_ARGB8888, &linear_modifier, 1,
        GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
    if (!bo) {
        bo = gbm_bo_create(gbm, width, height, DRM_FORMAT_ARGB8888,
                           GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT |
                               GBM_BO_USE_LINEAR);
    }
    if (!bo) {
        fprintf(stderr, "gpu_probe: linear ARGB8888 GBM allocation failed\n");
        return false;
    }

    bool ok = false;
    uint64_t modifier = gbm_bo_get_modifier(bo);
    int planes = gbm_bo_get_plane_count(bo);
    if (modifier != DRM_FORMAT_MOD_LINEAR && modifier != DRM_FORMAT_MOD_INVALID) {
        fprintf(stderr, "gpu_probe: non-linear modifier=0x%llx\n",
                (unsigned long long)modifier);
        goto out_bo;
    }
    if (planes != 1) {
        fprintf(stderr, "gpu_probe: unsupported plane_count=%d\n", planes);
        goto out_bo;
    }

    int dma_fd = get_fd_for_plane(bo, 0);
    if (dma_fd < 0) {
        fprintf(stderr, "gpu_probe: DMA-BUF export failed errno=%d\n", errno);
        goto out_bo;
    }
    uint32_t pitch = gbm_bo_get_stride(bo);
    size_t map_size = (size_t)pitch * height;
    void *mapped = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        dma_fd, 0);
    if (mapped != MAP_FAILED) {
        memset(mapped, 0x5a, map_size);
        munmap(mapped, map_size);
        puts("gpu_probe: cpu_map=dmabuf_mmap");
    } else {
        int mmap_errno = errno;
        struct gbm_import_fd_data import_data = {
            .fd = dma_fd,
            .width = width,
            .height = height,
            .stride = pitch,
            .format = DRM_FORMAT_ARGB8888,
        };
        struct gbm_bo *imported = gbm_bo_import(
            gbm, GBM_BO_IMPORT_FD, &import_data,
            GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (!imported) {
            fprintf(stderr,
                    "gpu_probe: DMA-BUF mmap failed errno=%d and GBM import failed\n",
                    mmap_errno);
            close(dma_fd);
            goto out_bo;
        }

        uint32_t map_stride = 0;
        void *map_data = NULL;
        mapped = gbm_bo_map(imported, 0, 0, width, height,
                            GBM_BO_TRANSFER_READ_WRITE, &map_stride, &map_data);
        if (!mapped || map_stride < width * 4) {
            fprintf(stderr,
                    "gpu_probe: DMA-BUF mmap failed errno=%d and gbm_bo_map failed stride=%u\n",
                    mmap_errno, map_stride);
            gbm_bo_destroy(imported);
            close(dma_fd);
            goto out_bo;
        }
        for (uint32_t row = 0; row < height; ++row)
            memset((uint8_t *)mapped + (size_t)row * map_stride, 0x5a,
                   width * 4);
        gbm_bo_unmap(imported, map_data);
        gbm_bo_destroy(imported);
        puts("gpu_probe: cpu_map=gbm_bo_map");
    }

    uint32_t handle = 0;
    if (drmPrimeFDToHandle(drm_fd, dma_fd, &handle) != 0) {
        fprintf(stderr, "gpu_probe: drmPrimeFDToHandle failed errno=%d\n", errno);
        close(dma_fd);
        goto out_bo;
    }
    uint32_t handles[4] = { handle, 0, 0, 0 };
    uint32_t pitches[4] = { pitch, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    uint32_t framebuffer = 0;
    if (drmModeAddFB2(drm_fd, width, height, DRM_FORMAT_ARGB8888,
                      handles, pitches, offsets, &framebuffer, 0) != 0) {
        fprintf(stderr, "gpu_probe: drmModeAddFB2 failed errno=%d\n", errno);
    } else {
        drmModeRmFB(drm_fd, framebuffer);
        ok = true;
    }
    struct drm_gem_close close_request = { .handle = handle };
    drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_request);
    close(dma_fd);

out_bo:
    gbm_bo_destroy(bo);
    return ok;
}

int main(int argc, char **argv)
{
    const char *drm_path = argc > 1 ? argv[1] : "/dev/dri/card0";
    int drm_fd = open(drm_path, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        fprintf(stderr, "gpu_probe: open %s failed errno=%d\n", drm_path, errno);
        return 10;
    }
    struct gbm_device *gbm = gbm_create_device(drm_fd);
    if (!gbm) {
        fprintf(stderr, "gpu_probe: gbm_create_device failed\n");
        close(drm_fd);
        return 11;
    }

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
            "eglGetPlatformDisplayEXT");
    EGLDisplay display = get_platform_display
        ? get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, NULL)
        : eglGetDisplay((EGLNativeDisplayType)gbm);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
        fprintf(stderr, "gpu_probe: EGL initialize failed error=0x%x\n",
                eglGetError());
        gbm_device_destroy(gbm);
        close(drm_fd);
        return 12;
    }

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    EGLConfig config = NULL;
    EGLint config_count = 0;
    if (!eglChooseConfig(display, config_attributes, &config, 1, &config_count) ||
        config_count != 1) {
        fprintf(stderr, "gpu_probe: EGL config unavailable\n");
        eglTerminate(display);
        gbm_device_destroy(gbm);
        close(drm_fd);
        return 13;
    }
    const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
    };
    const EGLint surface_attributes[] = {
        EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE
    };
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                                          context_attributes);
    EGLSurface surface = eglCreatePbufferSurface(display, config,
                                                 surface_attributes);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(display, surface, surface, context)) {
        fprintf(stderr, "gpu_probe: EGL context/surface failed error=0x%x\n",
                eglGetError());
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglTerminate(display);
        gbm_device_destroy(gbm);
        close(drm_fd);
        return 14;
    }

    const char *egl_vendor = eglQueryString(display, EGL_VENDOR);
    const char *gl_vendor = (const char *)glGetString(GL_VENDOR);
    const char *gl_renderer = (const char *)glGetString(GL_RENDERER);
    const char *gl_version = (const char *)glGetString(GL_VERSION);
    printf("gpu_probe: EGL=%d.%d egl_vendor=%s gl_vendor=%s renderer=%s version=%s\n",
           major, minor, egl_vendor ? egl_vendor : "unknown",
           gl_vendor ? gl_vendor : "unknown",
           gl_renderer ? gl_renderer : "unknown",
           gl_version ? gl_version : "unknown");
    bool mali = contains_case_insensitive(egl_vendor, "arm") ||
                contains_case_insensitive(gl_vendor, "arm") ||
                contains_case_insensitive(gl_renderer, "mali");
    bool gles_ok = mali && test_gles();
    bool scanout_ok = gles_ok && test_linear_scanout(gbm, drm_fd);

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    gbm_device_destroy(gbm);
    close(drm_fd);
    if (!mali) {
        fprintf(stderr, "gpu_probe: renderer is not ARM/Mali\n");
        return 15;
    }
    if (!gles_ok)
        return 16;
    if (!scanout_ok)
        return 17;
    puts("gpu_probe: result=ok linear_dmabuf=1 cpu_map=1 scanout=1");
    return 0;
}
