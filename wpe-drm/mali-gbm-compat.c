/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

struct gbm_bo;
struct gbm_device;

typedef struct gbm_bo *(*CreateWithModifiersFn)(struct gbm_device *, uint32_t,
                                                uint32_t, uint32_t,
                                                const uint64_t *, unsigned int);
typedef int (*GetFdPerPlaneFn)(struct gbm_bo *, int);
typedef struct gbm_device *(*CreateDeviceFn)(int);
typedef void (*DestroyDeviceFn)(struct gbm_device *);
typedef int (*DeviceGetFdFn)(struct gbm_device *);

struct DeviceFDEntry {
    struct gbm_device *device;
    int fd;
    int owned;
};

static pthread_mutex_t device_fd_lock = PTHREAD_MUTEX_INITIALIZER;
static struct DeviceFDEntry device_fds[8];

static struct DeviceFDEntry *find_device_fd(struct gbm_device *device)
{
    unsigned int i;

    for (i = 0; i < sizeof(device_fds) / sizeof(device_fds[0]); ++i) {
        if (device_fds[i].device == device)
            return &device_fds[i];
    }
    return NULL;
}

static void remember_device_fd(struct gbm_device *device, int fd, int owned)
{
    struct DeviceFDEntry *entry;
    unsigned int i;

    pthread_mutex_lock(&device_fd_lock);
    entry = find_device_fd(device);
    if (!entry) {
        for (i = 0; i < sizeof(device_fds) / sizeof(device_fds[0]); ++i) {
            if (!device_fds[i].device) {
                entry = &device_fds[i];
                break;
            }
        }
    }
    if (entry) {
        entry->device = device;
        entry->fd = fd;
        entry->owned = owned;
    }
    pthread_mutex_unlock(&device_fd_lock);
}

struct gbm_device *gbm_create_device(int fd)
{
    static CreateDeviceFn create_device;
    struct gbm_device *device;

    if (!create_device)
        create_device = (CreateDeviceFn)dlsym(RTLD_NEXT, "gbm_create_device");
    if (!create_device)
        return NULL;

    device = create_device(fd);
    if (device)
        remember_device_fd(device, fd, 0);
    return device;
}

int gbm_device_get_fd(struct gbm_device *device)
{
    static DeviceGetFdFn get_fd;
    static int logged;
    struct DeviceFDEntry *entry;
    int fd;

    pthread_mutex_lock(&device_fd_lock);
    entry = find_device_fd(device);
    if (entry) {
        fd = entry->fd;
        pthread_mutex_unlock(&device_fd_lock);
        return fd;
    }
    pthread_mutex_unlock(&device_fd_lock);

    if (!get_fd)
        get_fd = (DeviceGetFdFn)dlsym(RTLD_NEXT, "gbm_device_get_fd");
    if (!get_fd)
        return -1;

    /*
     * This vendor GBM returns a fresh F_DUPFD_CLOEXEC descriptor on every
     * call, unlike Mesa's borrowed-fd API. WebKit calls this several times per
     * frame, so cache one fallback descriptor for devices not created through
     * the wrapper instead of exhausting the process fd table.
     */
    fd = get_fd(device);
    if (fd >= 0) {
        remember_device_fd(device, fd, 1);
        if (!logged) {
            logged = 1;
            fprintf(stderr, "mali-gbm-compat: gbm_device_get_fd=borrowed\n");
        }
    }
    return fd;
}

void gbm_device_destroy(struct gbm_device *device)
{
    static DestroyDeviceFn destroy_device;
    struct DeviceFDEntry saved = { 0 };
    struct DeviceFDEntry *entry;

    pthread_mutex_lock(&device_fd_lock);
    entry = find_device_fd(device);
    if (entry) {
        saved = *entry;
        *entry = (struct DeviceFDEntry){ 0 };
    }
    pthread_mutex_unlock(&device_fd_lock);

    if (!destroy_device)
        destroy_device = (DestroyDeviceFn)dlsym(RTLD_NEXT, "gbm_device_destroy");
    if (destroy_device)
        destroy_device(device);
    if (saved.owned && saved.fd >= 0)
        close(saved.fd);
}

/*
 * The G52 userspace blob implements the older GBM entry points while current
 * WebKit calls their Mesa-era replacements. Keep this shim deliberately small:
 * GPU startup is rejected by wpe-gpu-probe if the translated calls do not
 * produce an exportable linear buffer.
 */
struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *device,
                                             uint32_t width,
                                             uint32_t height,
                                             uint32_t format,
                                             const uint64_t *modifiers,
                                             unsigned int count,
                                             uint32_t flags)
{
    static CreateWithModifiersFn create_with_modifiers;
    static int resolved;

    (void)flags;
    if (!resolved) {
        create_with_modifiers = (CreateWithModifiersFn)dlsym(
            RTLD_NEXT, "gbm_bo_create_with_modifiers");
        resolved = 1;
    }
    if (!create_with_modifiers) {
        fprintf(stderr, "mali-gbm-compat: gbm_bo_create_with_modifiers missing\n");
        return NULL;
    }
    return create_with_modifiers(device, width, height, format, modifiers, count);
}

int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane)
{
    static GetFdPerPlaneFn get_fd_per_plane;
    static int resolved;

    if (!resolved) {
        get_fd_per_plane = (GetFdPerPlaneFn)dlsym(
            RTLD_NEXT, "gbm_bo_get_fd_per_plane");
        resolved = 1;
    }
    if (!get_fd_per_plane) {
        fprintf(stderr, "mali-gbm-compat: gbm_bo_get_fd_per_plane missing\n");
        return -1;
    }
    return get_fd_per_plane(bo, plane);
}
