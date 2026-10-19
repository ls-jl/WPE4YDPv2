#include "RenderingFenceWait.h"
#include "DRMFramePixels.h"

#include <cassert>
#include <cstdio>
#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>

using namespace WPE::DRM;

struct Completion {
    bool called { false };
    FenceResult result { FenceResult::Pending };
};

static void completed(FenceResult result, gpointer data)
{
    auto* completion = static_cast<Completion*>(data);
    assert(!completion->called);
    completion->called = true;
    completion->result = result;
}

static void iterateUntil(GMainContext* context, const Completion& completion)
{
    auto deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
    while (!completion.called && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(context, FALSE);
        g_usleep(500);
    }
    assert(completion.called);
}

static void testFence()
{
    assert(probeRenderingFence(-1) == FenceResult::Ready);
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    assert(fd >= 0);
    assert(probeRenderingFence(fd) == FenceResult::Pending);
    auto* context = g_main_context_new();
    RenderingFenceWait wait;
    Completion completion;
    unsigned ticks = 0;
    auto* heartbeat = g_timeout_source_new(2);
    g_source_set_callback(heartbeat, [](gpointer data) -> gboolean {
        ++*static_cast<unsigned*>(data);
        return G_SOURCE_CONTINUE;
    }, &ticks, nullptr);
    g_source_attach(heartbeat, context);
    wait.start(fd, 30, completed, &completion, context);
    iterateUntil(context, completion);
    assert(completion.result == FenceResult::TimedOut);
    assert(ticks >= 4); // Waiting for GPU must not block other main-loop work.
    assert(!wait.active());
    g_source_destroy(heartbeat);
    g_source_unref(heartbeat);

    completion = Completion { };
    wait.start(fd, 100, completed, &completion, context);
    uint64_t value = 1;
    assert(write(fd, &value, sizeof(value)) == sizeof(value));
    iterateUntil(context, completion);
    assert(completion.result == FenceResult::Ready);
    assert(read(fd, &value, sizeof(value)) == sizeof(value));

    completion = Completion { };
    wait.start(fd, 1, completed, &completion, context);
    wait.cancel();
    assert(write(fd, &value, sizeof(value)) == sizeof(value));
    for (int i = 0; i < 10; ++i)
        g_main_context_iteration(context, FALSE);
    assert(!completion.called);
    close(fd);
    assert(probeRenderingFence(fd) == FenceResult::Failed);

    int pipeFDs[2];
    assert(!pipe(pipeFDs));
    assert(write(pipeFDs[1], &value, sizeof(value)) == sizeof(value));
    close(pipeFDs[1]);
    assert(probeRenderingFence(pipeFDs[0]) == FenceResult::Failed); // POLLIN|POLLHUP
    completion = Completion { };
    wait.start(pipeFDs[0], 100, completed, &completion, context);
    iterateUntil(context, completion);
    assert(completion.result == FenceResult::Failed);
    close(pipeFDs[0]);
    g_main_context_unref(context);
}

static void testMargins()
{
    const unsigned width = 12, height = 16, stride = 16;
    const unsigned rects[][4] = {
        { 0, 4, 12, 12 }, { 0, 0, 12, 12 }, // top/bottom toolbar
        { 4, 0, 8, 16 }, { 0, 0, 8, 16 }, // rotated toolbar
        { 2, 3, 7, 9 }, { 0, 0, 12, 16 }, { 12, 16, 0, 0 }
    };
    for (const auto& rect : rects) {
        std::vector<uint32_t> pixels(stride * height, 0x11223344);
        clearOutsideARGB8888(reinterpret_cast<uint8_t*>(pixels.data()), stride * 4,
            width, height, rect[0], rect[1], rect[2], rect[3], 0xff000000);
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < stride; ++x) {
                bool untouched = x >= width || (x >= rect[0] && x < rect[0] + rect[2]
                    && y >= rect[1] && y < rect[1] + rect[3]);
                assert(pixels[y * stride + x] == (untouched ? 0x11223344 : 0xff000000));
            }
        }
    }
}

int main()
{
    testFence();
    testMargins();
    puts("rendering fence and margin tests passed");
}
