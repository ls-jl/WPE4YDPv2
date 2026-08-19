#include "VideoReleaseQueue.h"

#include <cassert>

int main()
{
    WPE::DRM::VideoReleaseQueue queue(3);
    assert(queue.add(10) == WPE::DRM::VideoReleaseQueue::AddResult::Added);
    assert(queue.add(10) == WPE::DRM::VideoReleaseQueue::AddResult::Duplicate);
    assert(queue.add(11) == WPE::DRM::VideoReleaseQueue::AddResult::Added);
    assert(queue.add(12) == WPE::DRM::VideoReleaseQueue::AddResult::Added);
    assert(queue.add(13) == WPE::DRM::VideoReleaseQueue::AddResult::Overflow);
    assert(queue.front() == 10);
    queue.pop();
    assert(queue.front() == 11);
    queue.clear();
    assert(queue.empty());
    return 0;
}
