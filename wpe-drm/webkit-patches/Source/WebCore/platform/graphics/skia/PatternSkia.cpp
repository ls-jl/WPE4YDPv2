/*
 * Copyright (C) 2024 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "Pattern.h"

#if USE(SKIA)
#include "AffineTransform.h"
#include "ImageBuffer.h"
#include "NativeImage.h"
#include "PlatformDisplay.h"
#include "SkiaUtilities.h"

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkImage.h>
#include <skia/core/SkMatrix.h>
#include <skia/core/SkSamplingOptions.h>
#include <skia/core/SkTileMode.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

static bool useSkiaCPURasterCompositorForPattern()
{
    const char* value = getenv("WEBKIT_SKIA_CPU_COMPOSITOR");
    return value && *value && value[0] != '0';
}

sk_sp<SkShader> Pattern::createPlatformPattern(const SkSamplingOptions& samplingOptions, const sk_sp<GrContextThreadSafeProxy>& threadSafeGrContext) const
{
    auto nativeImage = tileNativeImage();
    if (!nativeImage)
        return nullptr;

    auto platformImage = nativeImage->platformImage();
    if (!platformImage)
        return nullptr;

    if (useSkiaCPURasterCompositorForPattern() && platformImage->isTextureBacked()) {
        auto& display = PlatformDisplay::sharedDisplay();
        auto* glContext = display.skiaGLContext();
        if (!glContext || !glContext->makeContextCurrent())
            return nullptr;

        auto* grContext = display.skiaGrContext();
        if (!grContext)
            return nullptr;

        auto imageInThisThread = platformImage->isValid(grContext->asRecorder()) ? platformImage : SkiaUtilities::rewrapImageForContext(grContext, *platformImage);
        if (!imageInThisThread)
            return nullptr;
        platformImage = imageInThisThread->makeRasterImage(grContext);
        if (!platformImage)
            return nullptr;
    } else if (threadSafeGrContext)
        platformImage = SkiaUtilities::createPromiseImageIfNeeded(platformImage, threadSafeGrContext);

    return platformImage->makeShader(repeatX() ? SkTileMode::kRepeat : SkTileMode::kDecal, repeatY() ? SkTileMode::kRepeat : SkTileMode::kDecal, samplingOptions, patternSpaceTransform());
}

} // namespace WebCore

#endif // USE(SKIA)
