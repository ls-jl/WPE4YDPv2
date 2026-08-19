/* compression_utils_portable.cc
 *
 * Copyright 2019 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the Chromium source repository LICENSE file.
 */

#include "compression_utils_portable.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

namespace zlib_internal {

const size_t kGzipZlibHeaderDifferenceBytes = 16;
const int kWindowBitsToGetGzipHeader = 16;
const int kZlibMemoryLevel = 8;

uLongf GzipExpectedCompressedSize(uLongf input_size)
{
  return kGzipZlibHeaderDifferenceBytes + compressBound(input_size);
}

uint32_t GetGzipUncompressedSize(const Bytef* compressed_data, size_t length)
{
  uint32_t size;
  if (length < sizeof(size))
    return 0;

  memcpy(&size, &compressed_data[length - sizeof(size)], sizeof(size));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return size;
#else
  return __builtin_bswap32(size);
#endif
}

inline int ZlibStreamWrapperType(WrapperType type)
{
  if (type == ZLIB)
    return MAX_WBITS;
  if (type == GZIP)
    return MAX_WBITS + kWindowBitsToGetGzipHeader;
  if (type == ZRAW)
    return -MAX_WBITS;
  return 0;
}

int GzipCompressHelper(Bytef* dest, uLongf* dest_length, const Bytef* source,
    uLong source_length, void* (*malloc_fn)(size_t), void (*free_fn)(void*))
{
  return CompressHelper(GZIP, dest, dest_length, source, source_length,
      Z_DEFAULT_COMPRESSION, malloc_fn, free_fn);
}

int CompressHelper(WrapperType wrapper_type, Bytef* dest, uLongf* dest_length,
    const Bytef* source, uLong source_length, int compression_level,
    void* (*malloc_fn)(size_t), void (*free_fn)(void*))
{
  if (compression_level < 0 || compression_level > 9)
    compression_level = Z_DEFAULT_COMPRESSION;

  z_stream stream;
  stream.next_in = static_cast<z_const Bytef*>(const_cast<Bytef*>(source));
  stream.avail_in = static_cast<uInt>(source_length);
  stream.next_out = dest;
  stream.avail_out = static_cast<uInt>(*dest_length);
  if (static_cast<uLong>(stream.avail_out) != *dest_length)
    return Z_BUF_ERROR;

  struct MallocFreeFunctions {
    void* (*malloc_fn)(size_t);
    void (*free_fn)(void*);
  } malloc_free = { malloc_fn, free_fn };

  if (malloc_fn) {
    if (!free_fn)
      return Z_BUF_ERROR;
    auto zalloc = [](void* opaque, uInt items, uInt size) {
      return reinterpret_cast<MallocFreeFunctions*>(opaque)->malloc_fn(items * size);
    };
    auto zfree = [](void* opaque, void* address) {
      return reinterpret_cast<MallocFreeFunctions*>(opaque)->free_fn(address);
    };
    stream.zalloc = static_cast<alloc_func>(zalloc);
    stream.zfree = static_cast<free_func>(zfree);
    stream.opaque = static_cast<voidpf>(&malloc_free);
  } else {
    stream.zalloc = static_cast<alloc_func>(0);
    stream.zfree = static_cast<free_func>(0);
    stream.opaque = static_cast<voidpf>(0);
  }

  int err = deflateInit2(&stream, compression_level, Z_DEFLATED,
      ZlibStreamWrapperType(wrapper_type), kZlibMemoryLevel, Z_DEFAULT_STRATEGY);
  if (err != Z_OK)
    return err;

  gz_header gzip_header;
  if (wrapper_type == GZIP) {
    memset(&gzip_header, 0, sizeof(gzip_header));
    err = deflateSetHeader(&stream, &gzip_header);
    if (err != Z_OK)
      return err;
  }

  err = deflate(&stream, Z_FINISH);
  if (err != Z_STREAM_END) {
    deflateEnd(&stream);
    return err == Z_OK ? Z_BUF_ERROR : err;
  }
  *dest_length = stream.total_out;
  return deflateEnd(&stream);
}

int GzipUncompressHelper(Bytef* dest, uLongf* dest_length, const Bytef* source,
    uLong source_length)
{
  return UncompressHelper(GZIP, dest, dest_length, source, source_length);
}

int UncompressHelper(WrapperType wrapper_type, Bytef* dest, uLongf* dest_length,
    const Bytef* source, uLong source_length)
{
  z_stream stream;
  stream.next_in = static_cast<z_const Bytef*>(const_cast<Bytef*>(source));
  stream.avail_in = static_cast<uInt>(source_length);
  if (static_cast<uLong>(stream.avail_in) != source_length)
    return Z_BUF_ERROR;

  stream.next_out = dest;
  stream.avail_out = static_cast<uInt>(*dest_length);
  if (static_cast<uLong>(stream.avail_out) != *dest_length)
    return Z_BUF_ERROR;

  stream.zalloc = static_cast<alloc_func>(0);
  stream.zfree = static_cast<free_func>(0);

  int err = inflateInit2(&stream, ZlibStreamWrapperType(wrapper_type));
  if (err != Z_OK)
    return err;

  err = inflate(&stream, Z_FINISH);
  if (err != Z_STREAM_END) {
    inflateEnd(&stream);
    if (err == Z_NEED_DICT || (err == Z_BUF_ERROR && !stream.avail_in))
      return Z_DATA_ERROR;
    return err;
  }
  *dest_length = stream.total_out;
  return inflateEnd(&stream);
}

} // namespace zlib_internal
