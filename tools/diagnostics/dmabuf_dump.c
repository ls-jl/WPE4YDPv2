#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static int remote_fd(int pid, int number)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_getfd)
    int process = syscall(SYS_pidfd_open, pid, 0);
    if (process >= 0) {
        int fd = syscall(SYS_pidfd_getfd, process, number, 0);
        close(process);
        if (fd >= 0)
            return fd;
    }
#endif
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd/%d", pid, number);
    return open(path, O_RDONLY | O_CLOEXEC);
}

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: %s PID FD WIDTH HEIGHT STRIDE OUTPUT.ppm\n", argv[0]);
        return 2;
    }
    int pid = atoi(argv[1]), number = atoi(argv[2]);
    int width = atoi(argv[3]), height = atoi(argv[4]), stride = atoi(argv[5]);
    if (pid <= 0 || number < 0 || width <= 0 || width > 16384
        || height <= 0 || height > 16384 || stride < width * 4
        || stride > 1024 * 1024)
        return 2;
    size_t size = (size_t)stride * (size_t)height;
    int fd = remote_fd(pid, number);
    if (fd < 0) {
        perror("duplicate source fd");
        return 1;
    }
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        perror("DMA_BUF_SYNC_START");
        close(fd);
        return 1;
    }
    unsigned char *pixels = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    int result = 1;
    if (pixels == MAP_FAILED)
        perror("mmap source");
    else {
        int output = open(argv[6], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        FILE *file = output >= 0 ? fdopen(output, "wb") : NULL;
        if (!file) {
            perror("open output");
            if (output >= 0)
                close(output);
        } else {
            result = fprintf(file, "P6\n%d %d\n255\n", width, height) < 0;
            for (int y = 0; !result && y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    unsigned char *pixel = pixels + (size_t)y * stride + x * 4;
                    unsigned char rgb[3] = { pixel[2], pixel[1], pixel[0] };
                    if (fwrite(rgb, 1, 3, file) != 3) {
                        result = 1;
                        break;
                    }
                }
            }
            if (fclose(file) != 0)
                result = 1;
        }
        munmap(pixels, size);
    }
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        perror("DMA_BUF_SYNC_END");
        result = 1;
    }
    close(fd);
    return result;
}
