#include "DMABufCPUAccess.h"
#include "RenderingFenceRetirement.h"
#include "DRMFrameLifecycle.h"
#include "RollingFrameTimings.h"

#include <cassert>
#include <cstdio>
#include <fcntl.h>
#include <sys/eventfd.h>

using namespace WPE::DRM;

struct SyncLog {
    std::vector<uint64_t> flags;
    int failure { 0 };
};
static int synchronize(int, uint64_t flags, void* data)
{
    auto& log = *static_cast<SyncLog*>(data);
    log.flags.push_back(flags);
    return log.failure;
}

static void accessOwnership()
{
    SyncLog log;
    {
        DMABufCPUAccess access(synchronize, &log);
        assert(access.begin(7, DMA_BUF_SYNC_RW));
        assert(access.begin(7, DMA_BUF_SYNC_RW));
        assert(log.flags.size() == 1);
        log.failure = EIO;
        assert(!access.finish());
        assert(access.active());
        assert(!access.begin(7, DMA_BUF_SYNC_RW));
        assert(!access.begin(8, DMA_BUF_SYNC_READ));
        log.failure = 0;
        assert(access.finish());
        assert(!access.active());
        assert(access.begin(8, DMA_BUF_SYNC_READ));
    }
    assert(log.flags.back() == (DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ));
    log.failure = EINVAL;
    DMABufCPUAccess access(synchronize, &log);
    assert(!access.begin(9, DMA_BUF_SYNC_WRITE));
    assert(!access.active());
}

struct BufferLifetime { unsigned released { 0 }; unsigned destroyed { 0 }; };
static void released(void* data) { ++static_cast<BufferLifetime*>(data)->released; }
static void destroyed(void* data) { ++static_cast<BufferLifetime*>(data)->destroyed; }

static void fenceOwnership()
{
    auto* context = g_main_context_new();
    BufferLifetime buffer;
    RenderingFenceRetirement retirement;
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    assert(fd >= 0);
    assert(retirement.retain(fd, &buffer, released, destroyed, context));
    for (unsigned i = 0; i < 20; ++i) g_main_context_iteration(context, FALSE);
    assert(!buffer.released && !buffer.destroyed && retirement.size() == 1);
    uint64_t value = 1;
    assert(write(fd, &value, sizeof(value)) == sizeof(value));
    g_main_context_iteration(context, FALSE);
    assert(buffer.released == 1);
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    assert(retirement.size() == 0 && buffer.destroyed == 1);

    int pipes[2];
    assert(!pipe(pipes));
    close(pipes[1]);
    assert(retirement.retain(pipes[0], &buffer, released, destroyed, context));
    for (unsigned i = 0; i < 20; ++i) g_main_context_iteration(context, FALSE);
    assert(buffer.released == 1 && retirement.size() == 1);
    assert(!retirement.finish(1));
    assert(buffer.destroyed == 1 && retirement.size() == 1);
    retirement.clear();
    assert(buffer.released == 1 && buffer.destroyed == 2);
    std::vector<BufferLifetime> blocked(5);
    for (unsigned i = 0; i < 5; ++i) {
        int waiting = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        bool retained = retirement.retain(waiting, &blocked[i], released, destroyed, context);
        assert(retained == (i < 4));
        if (!retained) close(waiting);
    }
    retirement.clear();
    for (unsigned i = 0; i < 4; ++i) assert(!blocked[i].released && blocked[i].destroyed == 1);

    BufferLifetime upload;
    int readFence = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    assert(retirement.retain(readFence, &upload, released, destroyed, context));
    assert(!retirement.finish(1));
    assert(!upload.released && !upload.destroyed);
    assert(write(readFence, &value, sizeof(value)) == sizeof(value));
    assert(retirement.finish(20));
    assert(upload.released == 1 && upload.destroyed == 1 && retirement.size() == 0);
    g_main_context_unref(context);
}

int main()
{
    RollingFrameTimings timings;
    timings.presented(1000000, 990000, true);
    timings.presented(1033333, 1023333, true);
    timings.presented(1066666, 1056666, true);
    assert(timings.percentile(1066666, 95, false) > 33.);
    assert(timings.percentile(1066666, 95, true) == 10.);
    timings.presented(1600000, 1590000, true);
    assert(timings.percentile(1600000, 99, false) > 500.);
    assert(timings.percentile(4000000, 95, false) == 0.);
    FrameTicket ticket;
    ticket.accept(17, 5, 3);
    assert(!ticket.advance(FramePhase::Submitted));
    assert(ticket.advance(FramePhase::WaitingFence));
    assert(ticket.advance(FramePhase::Preparing));
    assert(ticket.advance(FramePhase::Ready));
    assert(ticket.advance(FramePhase::Submitted));
    assert(!ticket.advance(FramePhase::Retired));
    assert(ticket.advance(FramePhase::Presented));
    assert(!ticket.complete(16));
    assert(ticket.complete(17));
    assert(!ticket.complete(17));
    assert(ticket.advance(FramePhase::Retired));
    accessOwnership();
    fenceOwnership();
    puts("DRM ownership tests passed: cache access, failed END, late GPU fence and bounded quarantine");
}
