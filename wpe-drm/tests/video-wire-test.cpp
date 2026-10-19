#include "VideoOverlayWire.h"
#include "VideoReleaseQueue.h"

#include <cassert>
#include <cstdio>
#include <cstring>

int main()
{
    VideoOverlayWireMessage frame;
    frame.type = VideoOverlayMessageFrame;
    frame.sequence = 7;
    assert(videoOverlayWireValid(frame, sizeof(frame)));
    assert(!videoOverlayWireValid(frame, sizeof(frame) - 1));
    frame.version = 2;
    assert(!videoOverlayWireValid(frame, sizeof(frame)));
    frame.version = 3;
    frame.messageSize = 0;
    assert(!videoOverlayWireValid(frame, sizeof(frame)));

    VideoCloseState close;
    close.request(12);
    assert(close.pending());
    assert(!close.acknowledge(11));
    assert(close.pending());
    assert(close.acknowledge(12));
    assert(!close.pending());
    assert(!close.acknowledge(12));

    WPE::DRM::VideoReleaseQueue messages;
    messages.add(7);
    messages.add(12, VideoOverlayMessageCloseComplete);
    assert(messages.front() == 7 && messages.type() == VideoOverlayMessageRelease);
    messages.pop();
    assert(messages.front() == 12 && messages.type() == VideoOverlayMessageCloseComplete);
    puts("Video v3 wire tests passed: length, version, ordered release and close confirmation");
}
