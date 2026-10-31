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

#include "DRMGeometry.h"

struct ChromeRenderState {
    bool enabled { true };
    bool visible { true };
    bool loading { false };
    double loadProgress { 0.0 };
    bool canBack { false };
    bool canForward { false };
    bool touchDebug { false };
    unsigned height { 44 };
    bool toolbarStacked { false };
    int addressX { 8 };
    int addressY { 6 };
    int addressWidth { 0 };
    int addressHeight { 32 };
    int controlsX { 0 };
    int controlsY { 6 };
    int controlsWidth { 0 };
    int controlsHeight { 32 };
    int buttonCount { 5 };
    gint64 transitionUS { 0 };
    unsigned tabCount { 1 };
    unsigned activeTab { 0 };
    char theme[8] { "light" };
    char panel[32] { "none" };
    char panelTitle[96] { };
    char touchDebugLabel[32] { "TOUCH DEBUG" };
    char homeLabel[32] { "HOME" };
    char url[256] { };
    char title[128] { };
    char lines[10][96] { };
    unsigned lineCount { 0 };
    int panelX { 0 };
    int panelY { 44 };
    int panelWidth { 0 };
    int panelHeight { 0 };
    int panelHeaderHeight { 0 };
    int panelFooterHeight { 0 };
    int panelRowHeight { 48 };
    double panelScrollOffset { 0 };
    double panelMaxScroll { 0 };
    bool panelCanAdd { true };
    int panelColumns { 3 };
    int panelRows { 2 };
    int panelPageCount { 1 };
    char itemIDs[10][32] { };
    bool itemEnabled[10] { true, true, true, true, true, true, true, true, true, true };
    bool lineEnabled[10] { };
    bool lineDanger[10] { };
    bool lineChecked[10] { };
    char lineKinds[10][16] { };
    char lineValues[10][96] { };
    char lineIcons[10][24] { };
    int lineSegmentCount[10] { };
    char lineSegmentLabels[10][3][64] { };
    bool lineSegmentEnabled[10][3] { };
    bool lineSegmentDanger[10][3] { };
    int pressedRow { -1 };
    int pressedSegment { -1 };
    int pressedControl { -1 };
};

struct ChromeStateCache {
    ChromeRenderState state;
    guint64 snapshotSequence { 0 };
    bool valid { false };
};

class ChromeRenderScope {
public:
    explicit ChromeRenderScope(ChromeStateCache&);
    ~ChromeRenderScope();
    ChromeRenderScope(const ChromeRenderScope&) = delete;
    ChromeRenderScope& operator=(const ChromeRenderScope&) = delete;
private:
    ChromeStateCache* m_previous;
};

bool chromeEnabled();
long chromeAnimationDurationMS();
ChromeStateCache& chromeStateCache();
bool refreshChromeRenderState(WPEView*);
const ChromeRenderState& cachedChromeRenderState();
bool nativeChromeCompositionRequired();
double chromeShownFraction(const ChromeRenderState& chrome);
uint32_t chromeReservedTopInset(uint32_t panelHeight);
uint32_t chromeFrameTopInset(uint32_t panelHeight, uint32_t sourceHeight);
bool chromeAnimationActive();
void drawChromeOverlay(uint8_t* destination, uint32_t destinationPitch, uint32_t destinationWidth, uint32_t destinationHeight, uint32_t panelWidth, uint32_t panelHeight, OutputRotation rotation);

size_t chromeFontCacheBytes();
void trimChromeFontCaches(bool critical);
