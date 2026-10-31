#ifndef PG_NATIVE_HTTP_RANGE_H
#define PG_NATIVE_HTTP_RANGE_H

// Portable validation for byte-range responses to immutable asset requests.
// For non-empty requests, only an exact 206 response is accepted. The caller
// must clamp requests to the manifest size before sending them; therefore 416
// is always an error. A zero-length request is a local no-op, not an HTTP call.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static inline bool pg_http_range_request_valid(uint64_t requested_offset,
                                               uint64_t requested_length,
                                               uint64_t expected_total) {
  return requested_length != 0 && expected_total != 0 &&
         requested_offset < expected_total &&
         requested_length <= expected_total - requested_offset;
}

static inline bool pg_http_range_decimal(const char **cursor, uint64_t *value) {
  const char *p = *cursor;
  if (*p < '0' || *p > '9') return false;
  uint64_t result = 0;
  do {
    unsigned digit = (unsigned)(*p - '0');
    if (result > (UINT64_MAX - digit) / 10) return false;
    result = result * 10 + digit;
    p++;
  } while (*p >= '0' && *p <= '9');
  *cursor = p;
  *value = result;
  return true;
}

static inline bool pg_http_range_is_ows(char c) {
  return c == ' ' || c == '\t';
}

static inline bool pg_http_range_ascii_equal_ci(char a, char b) {
  if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
  if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
  return a == b;
}

// Extract the complete length from either a satisfied or unsatisfied
// Content-Range field. Leading/trailing HTTP OWS is permitted; the grammar
// inside the field remains strict. The unit is case-insensitive per HTTP.
static inline bool pg_http_range_content_range_total(const char *field,
                                                     uint64_t *total) {
  if (!field || !total) return false;
  const char *begin = field;
  while (pg_http_range_is_ows(*begin)) begin++;
  const char *end = begin + strlen(begin);
  while (end > begin && pg_http_range_is_ows(end[-1])) end--;

  static const char unit[] = "bytes";
  for (size_t i = 0; i < sizeof(unit) - 1; i++) {
    if ((size_t)(end - begin) <= i ||
        !pg_http_range_ascii_equal_ci(begin[i], unit[i]))
      return false;
  }
  const char *p = begin + sizeof(unit) - 1;
  if (p >= end || *p++ != ' ') return false;

  uint64_t parsed_total;
  if (p < end && *p == '*') {
    // Unsatisfied byte-range: bytes */complete-length.
    p++;
    if (p >= end || *p++ != '/' ||
        !pg_http_range_decimal(&p, &parsed_total) || p != end)
      return false;
  } else {
    uint64_t start, last;
    if (!pg_http_range_decimal(&p, &start) || p >= end || *p++ != '-' ||
        !pg_http_range_decimal(&p, &last) || p >= end || *p++ != '/' ||
        !pg_http_range_decimal(&p, &parsed_total) || p != end ||
        last < start || parsed_total == 0 || last >= parsed_total)
      return false;
  }

  *total = parsed_total;
  return true;
}

// Accepts the Content-Range field value without its field name. `response_length`
// is the number of bytes in the response body; `expected_total` is the manifest
// size. A valid field has the exact form "bytes START-END/TOTAL". Unknown totals,
// extra tokens, overflow, and ranges not matching the request fail closed.
static inline bool pg_http_range_validate(int status_code,
                                          const char *content_range,
                                          uint64_t requested_offset,
                                          uint64_t requested_length,
                                          uint64_t expected_total,
                                          uint64_t response_length) {
  if (!requested_length) {
    return status_code == 0 && content_range == NULL && response_length == 0 &&
           requested_offset <= expected_total;
  }

  // Validate the requested interval before adding offset + length. Requests
  // crossing the manifest end are invalid; callers must clamp them first.
  if (status_code != 206 || !content_range ||
      !pg_http_range_request_valid(requested_offset, requested_length,
                                   expected_total)) return false;

  uint64_t wanted_end = requested_offset + requested_length - 1;
  uint64_t start, end, total;
  const char *p = content_range;
  while (pg_http_range_is_ows(*p)) p++;
  const char *field_end = p + strlen(p);
  while (field_end > p && pg_http_range_is_ows(field_end[-1])) field_end--;
  static const char unit[] = "bytes";
  for (size_t i = 0; i < sizeof(unit) - 1; i++) {
    if ((size_t)(field_end - p) <= i ||
        !pg_http_range_ascii_equal_ci(p[i], unit[i]))
      return false;
  }
  p += sizeof(unit) - 1;
  if (p >= field_end || *p++ != ' ') return false;
  if (!pg_http_range_decimal(&p, &start) || p >= field_end || *p++ != '-' ||
      !pg_http_range_decimal(&p, &end) || p >= field_end || *p++ != '/' ||
      !pg_http_range_decimal(&p, &total) || p != field_end) return false;

  // Exact range matching prevents a wrong but same-sized response from
  // poisoning the persistent packed cache. `*` totals are deliberately rejected.
  return start == requested_offset && end == wanted_end &&
         total == expected_total && end >= start && end < total &&
         response_length == end - start + 1;
}

// Production adapter: retain a well-formed server-reported total for the
// caller's manifest-mismatch handling, but accept/cache only an exact 206.
static inline bool pg_http_range_validate_response(int status_code,
                                                   const char *content_range,
                                                   uint64_t requested_offset,
                                                   uint64_t requested_length,
                                                   uint64_t expected_total,
                                                   uint64_t response_length,
                                                   uint64_t *actual_total) {
  uint64_t reported_total;
  if (pg_http_range_content_range_total(content_range, &reported_total) &&
      actual_total) *actual_total = reported_total;
  return pg_http_range_validate(status_code, content_range, requested_offset,
                                requested_length, expected_total,
                                response_length);
}

#endif
