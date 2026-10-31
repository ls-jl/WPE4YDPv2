/*
 * Copyright (C) 2023 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "DRMRenderingIncludes.h"

enum class OutputRotation : uint16_t {
    Rotate0 = 0,
    Rotate90 = 90,
    Rotate180 = 180,
    Rotate270 = 270
};

struct PanelSize {
    uint32_t width { 960 };
    uint32_t height { 266 };
};

bool parseSize(const char* value, uint32_t& width, uint32_t& height);
PanelSize configuredPanelSize();
int renderingFenceTimeoutMS();
PanelSize scanoutPanelForSource(uint32_t sourceWidth, uint32_t sourceHeight);
const char* drmFitMode();
uint32_t rotatedWidth(uint32_t width, uint32_t height, OutputRotation rotation);
uint32_t rotatedHeight(uint32_t width, uint32_t height, OutputRotation rotation);
bool parseRotation(const char* value, OutputRotation& rotation);
OutputRotation configuredOutputRotation();
uint32_t configuredMaxFPS();
uint64_t drmPlaneRotate0Value();
void fillARGB8888(uint8_t* destination, uint32_t destinationPitch, uint32_t width, uint32_t height, uint32_t color);
bool syncDMABuf(int fd, uint64_t flags, GError** error);
void writeRotatedPanelRow(const uint32_t* sourceRow, uint8_t* destination, uint32_t destinationPitch, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t panelY, uint32_t x0, uint32_t width);
drm_mode_rect rotatedDestRect(int x1, int y1, int x2, int y2, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t topInset);
void fillRotatedPanelRect(uint8_t* destination, uint32_t destinationPitch, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2, uint32_t color);
void rotateRegionARGB8888(const uint8_t* source, uint32_t sourceStride, uint8_t* destination, uint32_t destinationPitch, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation, uint32_t topInset, uint32_t x0, uint32_t y0, uint32_t width, uint32_t height);
void copyRotatedARGB8888(const uint8_t* source, uint32_t sourceWidth, uint32_t sourceHeight, uint32_t sourceStride, uint8_t* destination, uint32_t destinationPitch, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation);
