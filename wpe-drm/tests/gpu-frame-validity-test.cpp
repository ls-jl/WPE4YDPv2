#include "GPUFrameValidity.h"

#include <cassert>
#include <cstdio>

int main()
{
    WebKit::GPUFrameValidity validity;

    assert(validity.requiresFullRepaint(960, 266, true, 1));
    validity.didRender(960, 266, 1, true);
    assert(validity.initialized());
    assert(validity.lastFullGeneration() == 1);

    assert(!validity.requiresFullRepaint(960, 266, true, 2));
    assert(validity.requiresFullRepaint(960, 266, false, 2));
    assert(validity.requiresFullRepaint(266, 960, true, 2));
    assert(validity.requiresFullRepaint(960, 266, true, 1));

    validity.didRender(960, 266, 2, false);
    assert(validity.lastRenderedGeneration() == 2);
    assert(validity.lastFullGeneration() == 1);

    validity.invalidate();
    assert(validity.requiresFullRepaint(960, 266, true, 3));

    std::puts("gpu-frame-validity-test: passed");
    return 0;
}
