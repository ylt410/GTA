#ifndef PG_NATIVE_ASSET_RANGE_SEARCH_H
#define PG_NATIVE_ASSET_RANGE_SEARCH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// First range whose start is greater than `position`. The caller owns a
// monotonically ordered array and supplies the byte stride and start field.
// Keeping the search strided avoids allocating a second index for mod slots.
// memcpy also permits an unaligned start field without undefined behaviour.
static inline size_t pg_asset_range_upper_bound(const void *ranges, size_t count,
                                                size_t stride, size_t startField,
                                                uint64_t position) {
  size_t first = 0, last = count;
  const unsigned char *bytes = (const unsigned char *)ranges;
  while (first < last) {
    size_t middle = first + (last - first) / 2;
    uint64_t start;
    memcpy(&start, bytes + middle * stride + startField, sizeof start);
    if (start <= position) first = middle + 1;
    else last = middle;
  }
  return first;
}

#endif
