/*
  FastLZ - Byte-aligned LZ77 compression library
  Copyright (C) 2005-2020 Ariya Hidayat <ariya.hidayat@gmail.com>

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#ifndef FASTLZ_H
#define FASTLZ_H

#define FASTLZ_VERSION 0x000100
#define FASTLZ_VERSION_MAJOR 0
#define FASTLZ_VERSION_MINOR 1
#define FASTLZ_VERSION_REVISION 0
#define FASTLZ_VERSION_STRING "0.1.0"

#ifdef __cplusplus
extern "C" {
#endif

/**
  Compress a block of data in the input buffer and returns the size of
  compressed block. The size of input buffer is specified by length. The
  minimum input buffer size is 16.

  The output buffer must be at least 5% larger than the input buffer
  and can not be smaller than 66 bytes.

  If the input is not compressible, the return value might be larger than
  length (max 5% larger) or equal to 0.
*/
int fastlz_compress(const void* input, int length, void* output);

/**
  Compress a block of data in the input buffer with specified level:
  1: Fast compression, good for real-time
  2: Slower compression, higher ratio
*/
int fastlz_compress_level(int level, const void* input, int length, void* output);

/**
  Decompress a block of compressed data and returns the size of the
  decompressed block. If error occurs, e.g. the compressed data is
  corrupted or the output buffer is not large enough, then 0 (zero)
  will be returned.

  The output buffer must be large enough to hold the decompressed data.
*/
int fastlz_decompress(const void* input, int length, void* output, int maxout);

#ifdef __cplusplus
}
#endif

#endif /* FASTLZ_H */

