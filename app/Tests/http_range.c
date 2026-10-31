#include "../Source/NativeHTTPRange.h"
#include <assert.h>
#include <stdio.h>

static void accepts(int status, const char *header, uint64_t offset,
                    uint64_t length, uint64_t total, uint64_t body) {
  assert(pg_http_range_validate(status, header, offset, length, total, body));
}

static void rejects(int status, const char *header, uint64_t offset,
                    uint64_t length, uint64_t total, uint64_t body) {
  assert(!pg_http_range_validate(status, header, offset, length, total, body));
}

int main(void) {
  // Exact regular and final short ranges are valid.
  accepts(206, "bytes 4096-8191/10000", 4096, 4096, 10000, 4096);
  accepts(206, "bytes 8192-9999/10000", 8192, 1808, 10000, 1808);
  // HTTP unit comparison is case-insensitive and OWS may surround the field.
  accepts(206, "\tByTeS 4096-8191/10000 \t", 4096, 4096, 10000, 4096);

  // Wrong start/end, body length, or manifest total; unknown/malformed/missing
  // Content-Range; and status 200 must not be cached.
  rejects(206, "bytes 0-4095/10000", 4096, 4096, 10000, 4096);
  rejects(206, "bytes 4096-8190/10000", 4096, 4096, 10000, 4095);
  rejects(206, "bytes 4096-8191/10001", 4096, 4096, 10000, 4096);
  rejects(206, "bytes 4096-8191/*", 4096, 4096, 10000, 4096);
  rejects(206, "bytes 4096-8191/10000, bytes 0-1/10000", 4096, 4096, 10000, 4096);
  rejects(206, "bytes 4096-8191/10000x", 4096, 4096, 10000, 4096);
  rejects(206, "bytes 4096-8191/10000 extra", 4096, 4096, 10000, 4096);
  rejects(206, "bytes 4096 -8191/10000", 4096, 4096, 10000, 4096);
  rejects(206, "bytes 4096-10000/10000", 4096, 5905, 10000, 5905);
  rejects(206, "bytes 0-0/0", 0, 1, 1, 1);
  rejects(206, NULL, 4096, 4096, 10000, 4096);
  rejects(200, "bytes 4096-8191/10000", 4096, 4096, 10000, 4096);

  // Decimal overflow and UINT64_MAX arithmetic are rejected, not wrapped.
  rejects(206, "bytes 18446744073709551616-18446744073709551616/18446744073709551616",
          0, 1, UINT64_MAX, 1);
  rejects(206, "bytes 18446744073709551614-18446744073709551615/18446744073709551615",
          UINT64_MAX - 1, 2, UINT64_MAX, 2);
  rejects(206, "bytes 0-0/18446744073709551615", UINT64_MAX, 2, UINT64_MAX, 2);

  // The caller must clamp before sending; HTTP 416 is never success in this
  // manifest-bounded asset path. Zero length is explicitly a local no-op.
  rejects(206, "bytes 8192-9999/10000", 8192, 4096, 10000, 1808);
  rejects(416, "bytes */10000", 10000, 1, 10000, 0);
  rejects(416, NULL, 0, 1, 10000, 0);
  accepts(0, NULL, 10000, 0, 10000, 0);
  rejects(0, NULL, 10001, 0, 10000, 0);
  rejects(0, NULL, 0, 0, 10000, 1);
  rejects(206, "bytes 0-0/10000", 0, 0, 10000, 0);

  // Production uses this response adapter to retain a parseable reported
  // total for the existing immutable-manifest mismatch path while rejecting
  // the response itself. This is the same one-call wiring used in
  // NativeAssets.m, not just a test of the lower-level parser.
  uint64_t actual = 10000;
  assert(pg_http_range_validate_response(206, "bytes 4096-8191/12000",
                                         4096, 4096, 10000, 4096, &actual) == false);
  assert(actual == 12000);
  actual = 10000;
  assert(pg_http_range_validate_response(206, "Bytes 4096-8191/10000 ",
                                         4096, 4096, 10000, 4096, &actual));
  assert(actual == 10000);
  actual = 10000;
  assert(pg_http_range_validate_response(416, "bytes */12000", 4096, 4096,
                                         10000, 0, &actual) == false);
  assert(actual == 12000);
  actual = 10000;
  assert(pg_http_range_validate_response(206, "bytes 4096-8191/*",
                                         4096, 4096, 10000, 4096, &actual) == false);
  assert(actual == 10000);

  assert(pg_http_range_request_valid(0, 1, 1));
  assert(pg_http_range_request_valid(UINT64_MAX - 1, 1, UINT64_MAX));
  assert(!pg_http_range_request_valid(UINT64_MAX - 1, 2, UINT64_MAX));
  assert(!pg_http_range_request_valid(0, 0, 10000));
  assert(!pg_http_range_request_valid(10000, 1, 10000));

  puts("PASS: strict HTTP Content-Range validation");
  return 0;
}
