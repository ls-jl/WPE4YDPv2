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

#include "WPEViewDRM.h"

#include "ChromeMotionState.h"
#include "DRMUniquePtr.h"
#include "WPEDisplayDRMPrivate.h"
#include "WPEScreenDRMPrivate.h"
#include "WPEToplevelDRM.h"
#include "WPEBufferSHM.h"
#include "WPEViewDRMPrivate.h"
#include "VideoReleaseQueue.h"
#include "RenderingFenceWait.h"
#include "DRMFramePixels.h"
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <dlfcn.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <linux/dma-buf.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <vector>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <xf86drm.h>
#include <wtf/FastMalloc.h>
#include <wtf/OptionSet.h>
#include <wtf/RunLoop.h>
#include <wtf/SafeStrerror.h>
#include <wtf/Seconds.h>
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/WTFGType.h>

