"""Compile and execute only the instrumented production wasm2c Emit/EmitData bodies.

The two extracted functions run against synthetic memory and reservation/runtime stubs.
No game initialization, gameplay loop, or other engine function graph executes.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def function_body(source: str, index: int) -> str | None:
    pattern = re.compile(
        rf"(?m)^void w2c_game_f{index}\(w2c_game\* instance(?:, [^\n]*)?\) \{{$"
    )
    matches = list(pattern.finditer(source))
    if not matches:
        return None
    if len(matches) != 1:
        raise RuntimeError(f"generated shard has {len(matches)} definitions for f{index}")
    start = matches[0].start()
    end = source.find("\n}\n", matches[0].end())
    if end < 0:
        raise RuntimeError(f"generated f{index} has no closing brace")
    return source[start:end + 3]


def extract(generated: Path) -> tuple[str, str]:
    found: dict[int, str] = {}
    for path in sorted(generated.glob("game_*.c")):
        source = path.read_text()
        for index in (550, 552):
            if index not in found:
                body = function_body(source, index)
                if body is not None:
                    found[index] = body
        if len(found) == 2:
            break
    if len(found) != 2:
        missing = sorted({550, 552} - set(found))
        raise RuntimeError(f"missing generated packet writer(s): {missing}; run the instrumentation patch first")
    for index, body in found.items():
        if body.count("pg_td_packet(instance,") != 1:
            raise RuntimeError(f"f{index} does not contain exactly one production packet callback")
        if "FUNC_EPILOGUE;" not in body:
            raise RuntimeError(f"f{index} is not a complete generated body")
    return found[550], found[552]


HARNESS = r'''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef double f64;
typedef struct fake_memory { uint8_t *data; uint64_t size; } wasm_rt_shared_memory_t;
typedef struct w2c_game { wasm_rt_shared_memory_t *w2c_env_memory; } w2c_game;

#define FUNC_PROLOGUE ((void)0)
#define FUNC_EPILOGUE ((void)0)
static uint8_t guest[10u * 1024u * 1024u];
static wasm_rt_shared_memory_t memory = { guest, sizeof guest };
static w2c_game game = { &memory };
static const uint64_t kCursor = 6583408ull;
static const uint64_t kRingPointer = 8348872ull;
static const uint64_t kBatchCount = 8349592ull;
static const uint64_t kProducerLock = 8348864ull;
static uint64_t ringBase;
static uint32_t callbackEnabled, callbackCount, callbackGuestReads;
static uint64_t callbackAddress;
static uint32_t callbackOp, callbackWords, callbackSawLock, callbackSawPublished;
static uint32_t publishedCount, publishedHead;

static void range(uint64_t address, uint64_t length) {
  assert(address <= memory.size && length <= memory.size - address);
}
static uint32_t read32(uint64_t address) {
  uint32_t value; range(address, sizeof value); memcpy(&value, guest + address, sizeof value); return value;
}
static uint64_t read64(uint64_t address) {
  uint64_t value; range(address, sizeof value); memcpy(&value, guest + address, sizeof value); return value;
}
static void write32(uint64_t address, uint32_t value) {
  range(address, sizeof value); memcpy(guest + address, &value, sizeof value);
}
static void write64(uint64_t address, uint64_t value) {
  range(address, sizeof value); memcpy(guest + address, &value, sizeof value);
}
static uint64_t checked_add_u64(uint64_t a, uint64_t b) { return a + b; }

u32 w2c_game_f549(w2c_game *instance) { (void)instance; return 1; }
u32 w2c_game_f90735(w2c_game *instance, u64 address, u32 expected, f64 timeout) {
  (void)instance; (void)address; (void)expected; (void)timeout; return 0;
}
void w2c_game_f90737(w2c_game *instance, u64 address, u32 count) {
  (void)instance; (void)address; (void)count;
}

u64 w2c_game_f551(w2c_game *instance, u32 requestedBytes) {
  (void)requestedBytes;
  const uint64_t base = read64(kRingPointer);
  const uint64_t cursor = read32(kCursor);
  assert(base == ringBase && cursor == 64);
  return base + cursor;
}

void pg_td_packet(w2c_game *instance, u64 address) {
  assert(instance == &game);
  callbackCount++;
  callbackAddress = address;
  callbackSawLock = read32(kProducerLock) == 1;
  callbackSawPublished = publishedCount != 0;
  // Match the native fast gate: disabled instrumentation does not read guest packet memory.
  if (!callbackEnabled) return;
  callbackGuestReads += 2;
  callbackOp = read32(address);
  callbackWords = read32(address + 4);
}

u32 i32_load_shared(uint8_t *localBase, uint64_t localSize, wasm_rt_shared_memory_t *mem, u64 address) {
  (void)localBase; (void)localSize; assert(mem == &memory); return read32(address);
}
void i32_store_shared(uint8_t *localBase, uint64_t localSize, wasm_rt_shared_memory_t *mem, u64 address, u32 value) {
  (void)localBase; (void)localSize; assert(mem == &memory); write32(address, value);
}
u64 i64_load_shared(uint8_t *localBase, uint64_t localSize, wasm_rt_shared_memory_t *mem, u64 address) {
  (void)localBase; (void)localSize; assert(mem == &memory); return read64(address);
}
u64 i64_load8_u_shared(uint8_t *localBase, uint64_t localSize, wasm_rt_shared_memory_t *mem, u64 address) {
  (void)localBase; (void)localSize; assert(mem == &memory); range(address, 1); return guest[address];
}
u32 i32_atomic_load_shared(uint8_t *localBase, uint64_t localSize, wasm_rt_shared_memory_t *mem, u64 address) {
  return i32_load_shared(localBase, localSize, mem, address);
}
u32 i32_atomic_rmw_cmpxchg_shared(uint8_t *localBase, uint64_t localSize, wasm_rt_shared_memory_t *mem,
                                  u64 address, u32 expected, u32 replacement) {
  (void)localBase; (void)localSize; assert(mem == &memory);
  const uint32_t old = read32(address);
  if (old == expected) write32(address, replacement);
  return old;
}
u32 i32_atomic_rmw_xchg_shared(uint8_t *localBase, uint64_t localSize, wasm_rt_shared_memory_t *mem,
                               u64 address, u32 replacement) {
  (void)localBase; (void)localSize; assert(mem == &memory);
  const uint32_t old = read32(address); write32(address, replacement); return old;
}
void i32_atomic_store_shared(uint8_t *localBase, uint64_t localSize, wasm_rt_shared_memory_t *mem,
                             u64 address, u32 value) {
  (void)localBase; (void)localSize; assert(mem == &memory);
  if (address == ringBase) {
    assert(callbackCount == 1 && callbackSawLock && !callbackSawPublished);
    ++publishedCount;
    publishedHead = value;
  }
  write32(address, value);
}
void memory_copy(wasm_rt_shared_memory_t *dst, wasm_rt_shared_memory_t *src,
                 u64 dstAddress, u64 srcAddress, u64 length) {
  assert(dst == &memory && src == &memory); range(dstAddress, length); range(srcAddress, length);
  memmove(guest + dstAddress, guest + srcAddress, (size_t)length);
}
void memory_fill(wasm_rt_shared_memory_t *mem, u64 address, u32 value, u64 length) {
  assert(mem == &memory); range(address, length); memset(guest + address, (int)(uint8_t)value, (size_t)length);
}

/* Extracted unchanged from this build's generated C shard. */
__EMIT_BODY__

static void reset_case(void) {
  memset(guest, 0, sizeof guest);
  ringBase = 100000;
  write64(kRingPointer, ringBase);
  write64(8348856ull, ringBase);
  write32(kCursor, 64);
  write32(kBatchCount, 63); // The production writer publishes after this packet.
  write32(kProducerLock, 0);
  callbackEnabled = callbackCount = callbackGuestReads = 0;
  callbackAddress = callbackOp = callbackWords = callbackSawLock = callbackSawPublished = 0;
  publishedCount = publishedHead = 0;
}

static void expected_packet(uint32_t op, uint32_t headerWords, const uint8_t *payload,
                            size_t payloadBytes, const uint8_t *padding, size_t paddingBytes,
                            int emitData) {
  const uint64_t address = ringBase + 64;
  const size_t padded = (paddingBytes + 3u) & ~3u;
  const size_t total = 8u + payloadBytes + (emitData ? padded : 0u);
  uint8_t expected[128] = {0};
  assert(total <= sizeof expected);
  memcpy(expected, &op, 4); memcpy(expected + 4, &headerWords, 4);
  if (payloadBytes) memcpy(expected + 8, payload, payloadBytes);
  if (emitData && paddingBytes) memcpy(expected + 8 + payloadBytes, padding, paddingBytes);
  assert(memcmp(guest + address, expected, total) == 0);
  assert(callbackCount == 1 && callbackAddress == address);
  assert(callbackSawLock == 1 && callbackSawPublished == 0);
  assert(publishedCount == 1 && publishedHead == 64 + total);
  assert(read32(kProducerLock) == 0);
  assert(read32(kCursor) == 64 + total);
  assert(callbackGuestReads == (callbackEnabled ? 2u : 0u));
  if (callbackEnabled) {
    assert(callbackOp == op && callbackWords == headerWords);
  }
}

static void run_emit(uint32_t words, int enabled) {
  reset_case(); callbackEnabled = (uint32_t)enabled;
  const uint32_t op = 32;
  uint32_t data[6] = {0x10203040u, 0x55667788u, 0x99aabbccu, 0xdeadbeefu, 0x01020304u, 0xfedcba98u};
  const size_t bytes = (size_t)words * 4u;
  const uint64_t payloadAddress = 20000;
  if (bytes) memcpy(guest + payloadAddress, data, bytes);
  w2c_game_f550(&game, op, payloadAddress, words);
  expected_packet(op, words, (const uint8_t *)data, bytes, NULL, 0, 0);
}

static void run_emit_data(uint32_t words, size_t paddingBytes, int enabled) {
  reset_case(); callbackEnabled = (uint32_t)enabled;
  const uint32_t op = 32;
  uint32_t data[6] = {0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u, 0x01020304u, 0xa0b0c0d0u};
  uint8_t padding[17];
  for (size_t i = 0; i < sizeof padding; ++i) padding[i] = (uint8_t)(0xa0u + i);
  const size_t payloadBytes = (size_t)words * 4u;
  const uint64_t payloadAddress = 20000, paddingAddress = 21000;
  if (payloadBytes) memcpy(guest + payloadAddress, data, payloadBytes);
  if (paddingBytes) memcpy(guest + paddingAddress, padding, paddingBytes);
  const uint32_t headerWords = words + (uint32_t)((paddingBytes + 3u) / 4u);
  w2c_game_f552(&game, op, payloadAddress, words, paddingAddress, paddingBytes);
  expected_packet(op, headerWords, (const uint8_t *)data, payloadBytes, padding, paddingBytes, 1);
  // EmitData advances its internal payload cursor; the callback must still receive packet base.
  assert(callbackAddress != ringBase + 64 + 8 + payloadBytes + ((paddingBytes + 3u) & ~3u));
}

int main(void) {
  for (int enabled = 0; enabled <= 1; ++enabled) {
    run_emit(0, enabled);
    run_emit(6, enabled);
    const size_t paddings[] = {0, 1, 3, 4, 5, 17};
    for (size_t i = 0; i < sizeof paddings / sizeof paddings[0]; ++i) {
      run_emit_data(0, paddings[i], enabled);
      run_emit_data(6, paddings[i], enabled);
    }
  }
  puts("temporal packet production: OK");
  return 0;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generated", type=Path, required=True,
                        help="directory containing this build's instrumented Generated/game_*.c")
    parser.add_argument("--cc", default="cc", help="C compiler command (default: cc)")
    args = parser.parse_args()
    emit, emit_data = extract(args.generated)
    with tempfile.TemporaryDirectory(prefix="temporal-packet-production-") as directory:
        root = Path(directory)
        source = root / "packet_production.c"
        binary = root / "packet_production"
        source.write_text(HARNESS.replace("__EMIT_BODY__", emit + "\n" + emit_data))
        command = shlex.split(args.cc) + ["-std=c11", "-O0", str(source), "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
