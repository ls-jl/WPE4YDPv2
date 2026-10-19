/* SPDX-License-Identifier: MIT */
#pragma once

#include <cstdint>

namespace WPE { namespace DRM {

enum class FramePhase : uint8_t { Retired, Queued, WaitingFence, Preparing, Ready, Submitted, Presented };

struct FrameTicket {
    uint64_t serial { 0 };
    uintptr_t bufferID { 0 };
    uint64_t geometryEpoch { 0 };
    FramePhase phase { FramePhase::Retired };
    bool rendered { false };

    void accept(uint64_t nextSerial, uintptr_t buffer, uint64_t epoch)
    {
        serial = nextSerial;
        bufferID = buffer;
        geometryEpoch = epoch;
        phase = FramePhase::Queued;
        rendered = false;
    }

    bool advance(FramePhase next)
    {
        bool valid = (phase == FramePhase::Queued && (next == FramePhase::WaitingFence || next == FramePhase::Preparing))
            || (phase == FramePhase::WaitingFence && next == FramePhase::Preparing)
            || (phase == FramePhase::Preparing && next == FramePhase::Ready)
            || (phase == FramePhase::Ready && next == FramePhase::Submitted)
            || (phase == FramePhase::Submitted && next == FramePhase::Presented)
            || (next == FramePhase::Retired && phase != FramePhase::Submitted);
        if (valid)
            phase = next;
        return valid;
    }

    bool complete(uint64_t expectedSerial)
    {
        if (!serial || serial != expectedSerial || rendered)
            return false;
        rendered = true;
        return true;
    }
};

} }
