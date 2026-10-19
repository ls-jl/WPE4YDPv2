/* SPDX-License-Identifier: MIT */
#pragma once

#include <cerrno>
#include <cstdint>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>

namespace WPE { namespace DRM {

class DMABufCPUAccess {
public:
    using Synchronize = int (*)(int, uint64_t, void*);
    explicit DMABufCPUAccess(Synchronize synchronize = systemSynchronize, void* data = nullptr)
        : m_synchronize(synchronize), m_data(data) { }
    ~DMABufCPUAccess() { finish(); }
    DMABufCPUAccess(const DMABufCPUAccess&) = delete;
    DMABufCPUAccess& operator=(const DMABufCPUAccess&) = delete;

    bool begin(int fd, uint64_t direction)
    {
        if (m_active) {
            if (m_fd == fd && m_direction == direction && !m_error)
                return true;
            if (!finish())
                return false;
        }
        m_error = fd >= 0 ? m_synchronize(fd, DMA_BUF_SYNC_START | direction, m_data) : 0;
        if (m_error)
            return false;
        m_fd = fd;
        m_direction = direction;
        m_active = true;
        return true;
    }

    bool finish()
    {
        if (!m_active)
            return true;
        m_error = m_fd >= 0 ? m_synchronize(m_fd, DMA_BUF_SYNC_END | m_direction, m_data) : 0;
        if (m_error)
            return false; // Keep ownership until END really succeeds.
        m_active = false;
        m_fd = -1;
        return true;
    }

    bool active() const { return m_active; }
    int error() const { return m_error; }

private:
    static int systemSynchronize(int fd, uint64_t flags, void*)
    {
        dma_buf_sync sync { flags };
        int result;
        do {
            result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
        } while (result < 0 && errno == EINTR);
        return result < 0 ? errno : 0;
    }

    Synchronize m_synchronize;
    void* m_data;
    int m_fd { -1 };
    uint64_t m_direction { 0 };
    int m_error { 0 };
    bool m_active { false };
};

} }
