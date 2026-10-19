#include "TilePrefetchBudget.h"
#include <cassert>
#include <climits>
#include <cstdio>

int main()
{
    using WebCore::boundedVerticalTilePrefetch;
    assert(boundedVerticalTilePrefetch(960, 266, 0, 480, 256, false) == 133);
    assert(boundedVerticalTilePrefetch(960, 266, 0, 480, 256, true) == 128);
    assert(boundedVerticalTilePrefetch(266, 960, 0, 266, 256, false) == 256);
    assert(boundedVerticalTilePrefetch(266, 960, -5, 266, 256, true) == 128);
    assert(boundedVerticalTilePrefetch(1210, 568, 0, 605, 256, false) == 256);
    assert(boundedVerticalTilePrefetch(1210, 568, 0, 605, 256, true) == 0);
    assert(boundedVerticalTilePrefetch(INT_MAX, INT_MAX, 0, INT_MAX, INT_MAX, false) == 0);
    assert(boundedVerticalTilePrefetch(0, 100, 0, 256, 256, false) == 0);
    puts("Tile prefetch budget passed: landscape, portrait, pressure and overflow");
}
