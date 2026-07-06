/*
 * Copyright (C) 2026 Igalia S.L
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * aint with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#pragma once

#if USE(GSTREAMER)

#include "GStreamerQuirks.h"

namespace WebCore {

// Rockchip 词典笔 KMS overlay 视频直出：hole-punch sink 把 mppvideodec 解出
// 的 NV12 dmabuf 通过 unix socket 发给 UI 进程（WPEViewDRM），由后者放到
// DRM overlay plane 上；页面在视频区域打透明洞。
class GStreamerHolePunchQuirkRockchip final : public GStreamerHolePunchQuirk {
public:
    const ASCIILiteral identifier() const final { return "RockchipHolePunch"_s; }

    GstElement* createHolePunchVideoSink(bool, const MediaPlayer*) final;
    bool setHolePunchVideoRectangle(GstElement*, const IntRect&) final;
};

} // namespace WebCore

#endif // USE(GSTREAMER)
