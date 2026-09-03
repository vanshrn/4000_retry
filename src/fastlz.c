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

#include "fastlz.h"
#include <stdint.h>

#define MAX_COPY 32
#define MAX_LEN 264 /* 256 + 8 */
#define MAX_DISTANCE 8192

#define FASTLZ_READU16(p) ((uint16_t)(((const uint8_t*)(p))[0] | (((const uint8_t*)(p))[1] << 8)))

#define HASH_LOG 13
#define HASH_SIZE (1 << HASH_LOG)
#define HASH_MASK (HASH_SIZE - 1)
#define HASH_FUNCTION(v, p) \
  v = FASTLZ_READU16(p); \
  v ^= FASTLZ_READU16(p + 1) ^ (v >> (16 - HASH_LOG)); \
  v &= HASH_MASK

#if defined(BOARD_HAS_PSRAM)
#define PSRAM_ATTR __attribute__((section(".ext_ram.bss")))
#else
#define PSRAM_ATTR
#endif

// Static hash table allocated in PSRAM BSS — zero stack consumption!
static const uint8_t* s_htab[HASH_SIZE] PSRAM_ATTR;

int fastlz_compress_level(int level, const void* input, int length, void* output) {
  const uint8_t* ip = (const uint8_t*)input;
  const uint8_t* ip_bound = ip + length - 2;
  const uint8_t* ip_limit = ip + length - 12;
  uint8_t* op = (uint8_t*)output;

  const uint8_t** hslot;
  uint32_t hval;

  uint32_t copy;

  /* sanity check */
  if (length < 16) {
    if (length == 0) return 0;
    /* literal copy only */
    *op++ = length - 1;
    ip_bound++;
    while (ip <= ip_bound) *op++ = *ip++;
    return length + 1;
  }

  /* initialize hash table */
  for (hslot = s_htab; hslot < s_htab + HASH_SIZE; hslot++) *hslot = ip;

  copy = 2;
  op[0] = MAX_COPY - 1;
  op[1] = ip[0];
  op[2] = ip[1];
  op += 3;
  ip += 2;

  /* main loop */
  while (ip < ip_limit) {
    const uint8_t* ref;
    uint32_t distance;
    uint32_t len = 3;
    const uint8_t* anchor = ip;

    /* find potential match */
    HASH_FUNCTION(hval, ip);
    hslot = s_htab + hval;
    ref = *hslot;
    *hslot = ip;
    /* calculate distance to the match */
    distance = anchor - ref;

    /* is this a match? */
    if (distance == 0 || (distance >= MAX_DISTANCE) ||
        *ref++ != *ip++ || *ref++ != *ip++ || *ref++ != *ip++) {
      /* literal copy */
      op[0] = *anchor++;
      op++;
      ip = anchor;
      copy++;
      if (copy == MAX_COPY) {
        copy = 0;
        op[0] = MAX_COPY - 1;
        op++;
      }
      continue;
    }

    /* match is found */
    if (copy) {
      /* write pending literal copies */
      op[-(int)(copy + 1)] = copy - 1;
    } else {
      op--;
    }
    copy = 0;

    /* find length of the match */
    ip_bound = anchor + length - (anchor - (const uint8_t*)input);
    if (ip_bound > anchor + MAX_LEN) ip_bound = anchor + MAX_LEN;

    while (ip < ip_bound) {
      if (*ref++ != *ip++) break;
    }
    len = ip - anchor;

    /* encode match length and distance */
    distance--;
    if (len < 7) {
      *op++ = (len << 5) + (distance >> 8);
      *op++ = distance & 255;
    } else {
      *op++ = (7 << 5) + (distance >> 8);
      *op++ = len - 7;
      *op++ = distance & 255;
    }

    /* update hash table */
    HASH_FUNCTION(hval, ip - 2);
    s_htab[hval] = ip - 2;

    /* start new literal copy */
    op[0] = MAX_COPY - 1;
  }

  /* write remaining bytes as literals */
  ip_bound = (const uint8_t*)input + length;
  while (ip < ip_bound) {
    *op++ = *ip++;
    copy++;
    if (copy == MAX_COPY) {
      copy = 0;
      op[0] = MAX_COPY - 1;
      op++;
    }
  }

  if (copy) {
    op[-(int)(copy + 1)] = copy - 1;
  } else {
    op--;
  }

  return op - (uint8_t*)output;
}

int fastlz_compress(const void* input, int length, void* output) {
  return fastlz_compress_level(1, input, length, output);
}

int fastlz_decompress(const void* input, int length, void* output, int maxout) {
  const uint8_t* ip = (const uint8_t*)input;
  const uint8_t* ip_limit = ip + length;
  uint8_t* op = (uint8_t*)output;
  uint8_t* op_limit = op + maxout;
  uint32_t ctrl = (*ip++) & 31;

  while (ip < ip_limit) {
    uint32_t len = ctrl >> 5;
    uint8_t* ref = op - ((ctrl & 31) << 8) - 1;

    if (ctrl >= 32) {
      uint32_t code = *ip++;
      if (len == 7) {
        len += code;
        code = *ip++;
      }
      ref -= code;
      len += 2;

      if (op + len > op_limit) return 0;

      if (ref < (uint8_t*)output) return 0;

      while (len--) *op++ = *ref++;
    } else {
      ctrl++;
      if (op + ctrl > op_limit) return 0;
      if (ip + ctrl > ip_limit) return 0;
      while (ctrl--) *op++ = *ip++;
    }

    if (op >= op_limit) break;
    ctrl = *ip++;
  }

  return op - (uint8_t*)output;
}

