/* compression_utils_portable.h
 *
 * Copyright 2019 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the Chromium source repository LICENSE file.
 */
#ifndef THIRD_PARTY_ZLIB_GOOGLE_COMPRESSION_UTILS_PORTABLE_H_
#define THIRD_PARTY_ZLIB_GOOGLE_COMPRESSION_UTILS_PORTABLE_H_

#include <stdint.h>

#if defined(USE_SYSTEM_ZLIB)
#include <zlib.h>
#else
#include "zlib.h"
#endif

namespace zlib_internal {

enum WrapperType {
  ZLIB,
  GZIP,
  ZRAW,
};

uLongf GzipExpectedCompressedSize(uLongf input_size);
uint32_t GetGzipUncompressedSize(const Bytef* compressed_data, size_t length);
int GzipCompressHelper(Bytef*, uLongf*, const Bytef*, uLong, void* (*)(size_t), void (*)(void*));
int CompressHelper(WrapperType, Bytef*, uLongf*, const Bytef*, uLong, int, void* (*)(size_t), void (*)(void*));
int GzipUncompressHelper(Bytef*, uLongf*, const Bytef*, uLong);
int UncompressHelper(WrapperType, Bytef*, uLongf*, const Bytef*, uLong);

} // namespace zlib_internal

#endif // THIRD_PARTY_ZLIB_GOOGLE_COMPRESSION_UTILS_PORTABLE_H_
