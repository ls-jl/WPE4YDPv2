/* SPDX-License-Identifier: MIT */
#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>

enum : uint32_t {
    VideoOverlayMessageFrame = 1,
    VideoOverlayMessageRect = 2,
    VideoOverlayMessageHide = 3,
    VideoOverlayMessageRelease = 4,
    VideoOverlayMessageCloseComplete = 5,
};
static constexpr uint32_t videoOverlayProtocolVersion = 3;

struct VideoOverlayWireMessage {
    uint32_t version { videoOverlayProtocolVersion };
    uint32_t type { 0 };
    uint64_t sequence { 0 };
    uint32_t fourcc { 0 };
    uint32_t width { 0 };
    uint32_t height { 0 };
    uint32_t planeCount { 0 };
    uint32_t strides[3] { };
    uint32_t offsets[3] { };
    int32_t rectX { 0 };
    int32_t rectY { 0 };
    int32_t rectWidth { 0 };
    int32_t rectHeight { 0 };
    uint64_t modifier { 0 };
    uint32_t messageSize { sizeof(VideoOverlayWireMessage) };
};
static_assert(std::is_trivially_copyable<VideoOverlayWireMessage>::value, "Video wire must remain byte-copyable");

inline bool videoOverlayWireValid(const VideoOverlayWireMessage& message, size_t length)
{
    return length == sizeof(message) && message.messageSize == sizeof(message)
        && message.version == videoOverlayProtocolVersion
        && message.type >= VideoOverlayMessageFrame && message.type <= VideoOverlayMessageCloseComplete;
}

class VideoCloseState {
public:
    void request(uint64_t sequence) { m_sequence = sequence; m_confirmed = false; }
    bool acknowledge(uint64_t sequence)
    {
        if (!m_sequence || sequence != m_sequence || m_confirmed)
            return false;
        m_confirmed = true;
        return true;
    }
    bool pending() const { return m_sequence && !m_confirmed; }
    uint64_t sequence() const { return m_sequence; }
private:
    uint64_t m_sequence { 0 };
    bool m_confirmed { false };
};
