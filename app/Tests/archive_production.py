#!/usr/bin/env python3
"""Compile a harness from the real current and build-23 archive production functions."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / "Source/EngineHost.c"
BASELINE = ROOT / "Tests/perf_baselines/EngineHost-archive-build23.c"


def extract_function(source, marker):
    start = source.index(marker)
    open_brace = source.index("{", start)
    depth = 0
    state = "code"
    escaped = False
    i = open_brace
    while i < len(source):
        c = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "line_comment":
            if c == "\n":
                state = "code"
        elif state == "block_comment":
            if c == "*" and nxt == "/":
                state = "code"
                i += 1
        elif state in ("string", "char"):
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif (state == "string" and c == '"') or (state == "char" and c == "'"):
                state = "code"
        elif c == "/" and nxt == "/":
            state = "line_comment"
            i += 1
        elif c == "/" and nxt == "*":
            state = "block_comment"
            i += 1
        elif c == '"':
            state = "string"
        elif c == "'":
            state = "char"
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
        i += 1
    raise ValueError(f"unterminated function starting at {marker!r}")


def rename_identifier(source, old, new):
    return re.sub(r"\b" + re.escape(old) + r"\b", new, source)


HARNESS = r'''#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "NativeArchiveLookup.h"

enum { MEMORY_BYTES = 4 * 1024 * 1024, PACKFILE_AT = 256, LABEL_AT = 512,
       ENTRY_TABLE_AT = 4096, NAMES_TABLE_AT = 1024 * 1024,
       MAX_PIECES = 512, MAX_BENCH_LEAVES = 8192 };
typedef uint64_t u64;
typedef uint32_t u32;
typedef struct w2c_game { int unused; } w2c_game;
typedef struct w2c_env { int unused; } w2c_env;
typedef struct PGModPiece {
    const char *inner;
    uint64_t offset;
    uint32_t length, low, high;
    int resource;
} PGModPiece;
typedef struct Node { const char *name; uint32_t first, count; int directory; } Node;

static uint8_t memory[MEMORY_BYTES];
static uint8_t initialMemory[MEMORY_BYTES];
static uint8_t expectedMemory[MEMORY_BYTES];
static struct w2c_env env;
static PGModPiece configuredPieces[MAX_PIECES];
static int configuredCount;
static _Atomic int pg_mods_reported_missing;
static uint64_t pg_mod_test_entry, pg_mod_test_value;
static int allocationCall, failAllocationAt;

static void *pg_pointer(struct w2c_env *host, uint64_t address, uint64_t size) {
    (void)host;
    if (address > MEMORY_BYTES || size > MEMORY_BYTES - address) abort();
    return memory + address;
}
static const char *pg_string(struct w2c_env *host, uint64_t address) {
    const char *text = (const char *)pg_pointer(host, address, 1);
    if (!memchr(text, 0, MEMORY_BYTES - address)) abort();
    return text;
}
static void pg_game_index(const char *label, uint64_t packfile) {
    (void)label; (void)packfile;
}
static int pg_audio_rel_walk(uint64_t entries, uint64_t names, unsigned shift, uint32_t index, int depth) {
    (void)entries; (void)names; (void)shift; (void)index; (void)depth; return 0;
}
static int pg_mods_for_archive(const char *label, const PGModPiece **pieces) {
    (void)label; *pieces = configuredPieces; return configuredCount;
}
static void pg_log(const char *line) { (void)line; }
static void *archive_test_malloc(size_t size) {
    ++allocationCall;
    if (allocationCall == failAllocationAt) return NULL;
    return malloc(size);
}
static void *archive_test_calloc(size_t count, size_t size) {
    ++allocationCall;
    if (allocationCall == failAllocationAt) return NULL;
    return calloc(count, size);
}

/* Exact extracted implementations, with only unique names for side-by-side calls. */
/* OLD_WALK */
/* NEW_WALK */
/* INDEX_WALK */
#define malloc archive_test_malloc
#define calloc archive_test_calloc
/* OLD_MOUNT */
/* NEW_MOUNT */
#undef malloc
#undef calloc

static uint64_t *entry_words(size_t index) {
    return (uint64_t *)(void *)(memory + ENTRY_TABLE_AT + index * 16);
}

static void set_piece(int index, const char *path, uint64_t offset, uint32_t length) {
    configuredPieces[index] = (PGModPiece){path, offset, length, 0x1234, 0x5678, 0};
}

static void init_archive(const Node *nodes, size_t count) {
    assert(count > 0 && count * 16 < NAMES_TABLE_AT - ENTRY_TABLE_AT);
    memset(memory, 0, sizeof memory);
    size_t namesUsed = 0;
    for (size_t i = 0; i < count; ++i) {
        assert(nodes[i].name);
        const size_t nameLength = strlen(nodes[i].name) + 1;
        assert(namesUsed <= UINT16_MAX && nameLength <= MEMORY_BYTES - NAMES_TABLE_AT - namesUsed);
        const uint16_t nameOffset = (uint16_t)namesUsed;
        memcpy(memory + NAMES_TABLE_AT + namesUsed, nodes[i].name, nameLength);
        namesUsed += nameLength;
        uint64_t *e = entry_words(i);
        e[0] = nameOffset;
        if (nodes[i].directory) {
            e[0] |= UINT64_C(0x7fffff) << 40;
            e[1] = nodes[i].first | (uint64_t)nodes[i].count << 32;
        } else {
            e[0] |= UINT64_C(17) << 40;
            e[1] = UINT64_C(0xfeedcafe);
        }
    }
    *(uint64_t *)(void *)(memory + PACKFILE_AT + 16) = NAMES_TABLE_AT;
    *(uint64_t *)(void *)(memory + PACKFILE_AT + 32) = ENTRY_TABLE_AT;
    *(uint8_t *)(void *)(memory + PACKFILE_AT + 184) = 0;
    strcpy((char *)memory + LABEL_AT, "x64/test.rpf");
    memcpy(initialMemory, memory, sizeof memory);
}

static void invoke_and_compare(int failAt) {
    memcpy(memory, initialMemory, sizeof memory);
    allocationCall = failAllocationAt = 0;
    atomic_store(&pg_mods_reported_missing, 0);
    pg_packfile_mounted_baseline(NULL, PACKFILE_AT, LABEL_AT, 1);
    memcpy(expectedMemory, memory, sizeof memory);

    memcpy(memory, initialMemory, sizeof memory);
    allocationCall = 0; failAllocationAt = failAt;
    atomic_store(&pg_mods_reported_missing, 0);
    pg_packfile_mounted(NULL, PACKFILE_AT, LABEL_AT, 1);
    failAllocationAt = 0;
    assert(memcmp(memory, expectedMemory, sizeof memory) == 0);
}

static void assert_patched(size_t index, uint64_t offset, uint32_t length) {
    const uint64_t *e = entry_words(index);
    if (((e[0] >> 40) & UINT64_C(0x7fffff)) != offset / 512 || (uint32_t)e[1] != length)
        fprintf(stderr, "entry %zu not patched as expected: low=%08x high=%016llx e1=%016llx wanted block=%llu length=%u\\n",
                index, (unsigned)(e[0] >> 40), (unsigned long long)e[0], (unsigned long long)e[1],
                (unsigned long long)(offset / 512), length);
    assert(((e[0] >> 40) & UINT64_C(0x7fffff)) == offset / 512);
    assert(((e[0] >> 16) & UINT64_C(0xffffff)) == 0);
    assert((uint32_t)e[1] == length);
}

static void test_batch_and_fallbacks(void) {
    static const Node nodes[] = {
        {"root-name-is-not-part-of-path", 1, 2, 1},
        {"Vehicles", 3, 3, 1},
        {"root-only.txt", 0, 0, 0},
        {"Oracle.yft", 0, 0, 0},
        {"ORACLE.YFT", 0, 0, 0},
        {"nested", 6, 1, 1},
        {"oracle.yft", 0, 0, 0},
    };
    init_archive(nodes, sizeof nodes / sizeof nodes[0]);
    configuredCount = 6;
    set_piece(0, "vehicles/oracle.yft", 512 * 2, 101);
    set_piece(1, "VEHICLES/ORACLE.YFT", 512 * 3, 202);
    set_piece(2, "vehicles/nested/oracle.yft", 512 * 4, 303);
    set_piece(3, "root-only.txt", 512 * 5, 404);
    set_piece(4, "vehicles/missing.yft", 512 * 6, 505);
    set_piece(5, "Vehicles/Oracle.yft", 512 * 7, 606);
    invoke_and_compare(0);
    assert_patched(3, 512 * 7, 606); /* duplicate requests apply in source order */
    assert(entry_words(4)[0] >> 40 == 17); /* duplicate DFS leaf keeps its original entry */
    assert_patched(6, 512 * 4, 303);
    assert_patched(2, 512 * 5, 404); /* root entry name is omitted */
    assert(entry_words(4)[1] == UINT64_C(0xfeedcafe));
    assert(entry_words(0)[1] == (UINT64_C(2) << 32 | 1));

    /* count == 1 uses the original single-search fallback without allocation. */
    configuredCount = 1;
    set_piece(0, "vehicles/oracle.yft", 512 * 9, 909);
    invoke_and_compare(0);
    assert_patched(3, 512 * 9, 909);

    /* Either allocation failure discards the partial batch and falls back. */
    configuredCount = 6;
    set_piece(0, "vehicles/oracle.yft", 512 * 2, 101);
    set_piece(1, "VEHICLES/ORACLE.YFT", 512 * 3, 202);
    set_piece(2, "vehicles/nested/oracle.yft", 512 * 4, 303);
    set_piece(3, "root-only.txt", 512 * 5, 404);
    set_piece(4, "vehicles/missing.yft", 512 * 6, 505);
    set_piece(5, "Vehicles/Oracle.yft", 512 * 7, 606);
    invoke_and_compare(1);
    assert_patched(3, 512 * 7, 606);
    invoke_and_compare(2);
    assert_patched(3, 512 * 7, 606);
}

static void test_overlong_path_restores_sibling(void) {
    static char longName[600];
    static char longTarget[620];
    memset(longName, 'x', sizeof longName - 1);
    longName[sizeof longName - 1] = 0;
    snprintf(longTarget, sizeof longTarget, "%s/bad.bin", longName);
    Node nodes[] = {
        {"root", 1, 2, 1},
        {longName, 3, 1, 1},
        {"after.txt", 0, 0, 0},
        {"bad.bin", 0, 0, 0},
    };
    init_archive(nodes, sizeof nodes / sizeof nodes[0]);
    configuredCount = 2;
    set_piece(0, longTarget, 512 * 2, 111);
    set_piece(1, "after.txt", 512 * 3, 222);
    memcpy(memory, initialMemory, sizeof memory);
    allocationCall = failAllocationAt = 0;
    pg_packfile_mounted(NULL, PACKFILE_AT, LABEL_AT, 1);
    assert(entry_words(3)[0] >> 40 == 17); /* the overlong target was not entered */
    assert_patched(2, 512 * 3, 222); /* and the next sibling starts from the restored parent path */
}

static void test_depth_32_boundary(void) {
    static char names[36][16];
    Node nodes[36] = {{0}};
    nodes[0] = (Node){"root", 1, 1, 1};
    for (int depth = 1; depth <= 32; ++depth) {
        snprintf(names[depth], sizeof names[depth], "d%d", depth);
        nodes[depth] = (Node){names[depth], (uint32_t)(depth + 1), 1, 1};
    }
    nodes[32].count = 2;
    nodes[32].first = 33;
    nodes[33] = (Node){"boundary.bin", 0, 0, 0};
    nodes[34] = (Node){"d33", 35, 1, 1};
    nodes[35] = (Node){"too-deep.bin", 0, 0, 0};
    init_archive(nodes, 36);
    static char boundaryPath[160];
    boundaryPath[0] = 0;
    for (int depth = 1; depth <= 32; ++depth) {
        if (depth > 1) strcat(boundaryPath, "/");
        strcat(boundaryPath, names[depth]);
    }
    strcat(boundaryPath, "/boundary.bin");
    static char tooDeepPath[160];
    snprintf(tooDeepPath, sizeof tooDeepPath, "%s/d33/too-deep.bin", boundaryPath);
    char *leafSuffix = strstr(tooDeepPath, "/boundary.bin");
    assert(leafSuffix); *leafSuffix = 0;
    strcat(tooDeepPath, "/d33/too-deep.bin");
    configuredCount = 2;
    set_piece(0, boundaryPath, 512 * 4, 444);
    set_piece(1, tooDeepPath, 512 * 5, 555);
    memcpy(memory, initialMemory, sizeof memory);
    allocationCall = failAllocationAt = 0;
    pg_packfile_mounted(NULL, PACKFILE_AT, LABEL_AT, 1);
    assert_patched(33, 512 * 4, 444);
    assert(entry_words(35)[0] >> 40 == 17); /* depth 33 directory is pruned */
}

static size_t read_index_output(FILE *out, char *buffer, size_t capacity) {
    assert(capacity > 0);
    assert(fflush(out) == 0);
    rewind(out);
    const size_t count = fread(buffer, 1, capacity - 1, out);
    assert(!ferror(out));
    buffer[count] = 0;
    return count;
}

static void test_index_walk_bounded_paths(void) {
    char output[2048];
    static const Node normal[] = {
        {"root", 1, 2, 1},
        {"Vehicles", 3, 1, 1},
        {"root-only.txt", 0, 0, 0},
        {"Oracle.yft", 0, 0, 0},
    };
    init_archive(normal, sizeof normal / sizeof normal[0]);
    FILE *out = tmpfile(); assert(out);
    pg_index_walk(out, "archive", ENTRY_TABLE_AT, NAMES_TABLE_AT, 0, 0, output, 0, 0);
    read_index_output(out, output, sizeof output);
    assert(strcmp(output, "archive/Vehicles/Oracle.yft\narchive/root-only.txt\n") == 0);
    fclose(out);

    static char longName[600];
    memset(longName, 'z', sizeof longName - 1);
    longName[sizeof longName - 1] = 0;
    const Node overlong[] = {
        {"root", 1, 2, 1},
        {longName, 3, 1, 1},
        {"after.txt", 0, 0, 0},
        {"unreachable.txt", 0, 0, 0},
    };
    init_archive(overlong, sizeof overlong / sizeof overlong[0]);
    out = tmpfile(); assert(out);
    pg_index_walk(out, "archive", ENTRY_TABLE_AT, NAMES_TABLE_AT, 0, 0, output, 0, 0);
    read_index_output(out, output, sizeof output);
    assert(strcmp(output, "archive/after.txt\n") == 0);
    fclose(out);

    static char depthNames[35][16];
    Node deep[35] = {{0}};
    deep[0] = (Node){"root", 1, 1, 1};
    for (int depth = 1; depth <= 31; ++depth) {
        snprintf(depthNames[depth], sizeof depthNames[depth], "d%d", depth);
        deep[depth] = (Node){depthNames[depth], (uint32_t)(depth + 1), 1, 1};
    }
    deep[31] = (Node){depthNames[31], 32, 2, 1};
    deep[32] = (Node){"boundary.bin", 0, 0, 0};
    deep[33] = (Node){"d32", 34, 1, 1};
    deep[34] = (Node){"too-deep.bin", 0, 0, 0};
    init_archive(deep, 35);
    out = tmpfile(); assert(out);
    pg_index_walk(out, "archive", ENTRY_TABLE_AT, NAMES_TABLE_AT, 0, 0, output, 0, 0);
    read_index_output(out, output, sizeof output);
    char expected[1024] = "archive";
    for (int depth = 1; depth <= 31; ++depth) {
        strcat(expected, "/");
        strcat(expected, depthNames[depth]);
    }
    strcat(expected, "/boundary.bin\n");
    assert(strcmp(output, expected) == 0);
    fclose(out);
}

static uint64_t now_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) abort();
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static void benchmark(int requestedTargets, int runs) {
    static Node nodes[MAX_BENCH_LEAVES + 1];
    static char leafNames[MAX_BENCH_LEAVES][8];
    static uint64_t oldTimes[31], newTimes[31];
    assert(runs > 0 && runs <= 31);
    for (int i = 0; i <= MAX_BENCH_LEAVES; ++i) {
        if (i == 0) nodes[i] = (Node){"root", 1, MAX_BENCH_LEAVES, 1};
        else {
            snprintf(leafNames[i - 1], sizeof leafNames[i - 1], "f%x", i - 1);
            nodes[i] = (Node){leafNames[i - 1], 0, 0, 0};
        }
    }
    init_archive(nodes, MAX_BENCH_LEAVES + 1);
    configuredCount = requestedTargets;
    for (int i = 0; i < configuredCount; ++i) {
        const int leaf = 1 + (int)((uint64_t)i * MAX_BENCH_LEAVES / configuredCount);
        set_piece(i, leafNames[leaf - 1], (uint64_t)(i + 1) * 512, (uint32_t)(100 + i));
    }

    for (int run = 0; run < runs; ++run) {
        for (int order = 0; order < 2; ++order) {
            const int useNew = (run & 1) ? order == 0 : order == 1;
            memcpy(memory, initialMemory, sizeof memory);
            allocationCall = failAllocationAt = 0;
            const uint64_t start = now_ns();
            if (useNew) pg_packfile_mounted(NULL, PACKFILE_AT, LABEL_AT, 1);
            else pg_packfile_mounted_baseline(NULL, PACKFILE_AT, LABEL_AT, 1);
            const uint64_t elapsed = now_ns() - start;
            if (useNew) newTimes[run] = elapsed;
            else oldTimes[run] = elapsed;
            for (int i = 0; i < configuredCount; ++i) {
                const int leaf = 1 + (int)((uint64_t)i * MAX_BENCH_LEAVES / configuredCount);
                assert_patched((size_t)leaf, (uint64_t)(i + 1) * 512, (uint32_t)(100 + i));
            }
        }
    }
    /* Sort tiny sample vectors to report medians robustly. */
    for (int i = 0; i < runs; ++i) for (int j = i + 1; j < runs; ++j) {
        if (oldTimes[j] < oldTimes[i]) { uint64_t t = oldTimes[i]; oldTimes[i] = oldTimes[j]; oldTimes[j] = t; }
        if (newTimes[j] < newTimes[i]) { uint64_t t = newTimes[i]; newTimes[i] = newTimes[j]; newTimes[j] = t; }
    }
    printf("{\"targets\":%d,\"leaves\":%d,\"runs\":%d,\"old_full_dfs_ns\":%llu,\"batch_ns\":%llu}\n",
           requestedTargets, MAX_BENCH_LEAVES, runs,
           (unsigned long long)oldTimes[runs / 2], (unsigned long long)newTimes[runs / 2]);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--benchmark")) {
        const int runs = argc > 2 ? atoi(argv[2]) : 7;
        benchmark(1, runs); benchmark(8, runs); benchmark(64, runs); benchmark(512, runs);
        return 0;
    }
    if (argc > 1) {
        const int targets = atoi(argv[1]);
        const int runs = argc > 2 ? atoi(argv[2]) : 7;
        assert(targets == 1 || targets == 8 || targets == 64 || targets == 512);
        benchmark(targets, runs);
        return 0;
    }
    test_batch_and_fallbacks();
    test_overlong_path_restores_sibling();
    test_depth_32_boundary();
    test_index_walk_bounded_paths();
    puts("archive production integration passed");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmark", action="store_true", help="also compare production old/new mount paths")
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--targets", type=int, choices=(1, 8, 64, 512), help="benchmark a specific target count")
    parser.add_argument("--sanitize", action="store_true", help="build the harness with AddressSanitizer and UBSan")
    args = parser.parse_args()
    if not 1 <= args.runs <= 31:
        parser.error('--runs must be between 1 and 31')

    current = ENGINE.read_text()
    baseline = BASELINE.read_text()
    old_walk = rename_identifier(extract_function(baseline, "static uint32_t pg_rpf_walk("),
                                 "pg_rpf_walk", "pg_rpf_walk_baseline")
    new_walk = extract_function(current, "static uint32_t pg_rpf_walk(")
    index_walk = extract_function(current, "static void pg_index_walk(")
    old_mount = rename_identifier(extract_function(baseline, "void pg_packfile_mounted("),
                                  "pg_rpf_walk", "pg_rpf_walk_baseline")
    old_mount = rename_identifier(old_mount, "pg_packfile_mounted", "pg_packfile_mounted_baseline")
    new_mount = extract_function(current, "void pg_packfile_mounted(")
    harness = HARNESS.replace("/* OLD_WALK */", old_walk)
    harness = harness.replace("/* NEW_WALK */", new_walk)
    harness = harness.replace("/* INDEX_WALK */", index_walk)
    harness = harness.replace("/* OLD_MOUNT */", old_mount)
    harness = harness.replace("/* NEW_MOUNT */", new_mount)

    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SystemExit("A C compiler is required")
    with tempfile.TemporaryDirectory(prefix="archive-production-") as directory:
        source = Path(directory) / "archive_production_harness.c"
        binary = Path(directory) / "archive_production_harness"
        source.write_text(harness)
        command = [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        command += ["-I", str(ROOT / "Source"), str(source), "-o", str(binary)]
        subprocess.run(command, check=True)
        run_env = os.environ.copy()
        if args.sanitize:
            # LeakSanitizer cannot inspect /proc in this managed executor;
            # ASan and UBSan remain enabled for the generated harness.
            run_env["ASAN_OPTIONS"] = "detect_leaks=0"
        if args.targets:
            subprocess.run([str(binary), str(args.targets), str(args.runs)], check=True, env=run_env)
        else:
            subprocess.run([str(binary)], check=True, env=run_env)
            if args.benchmark:
                subprocess.run([str(binary), "--benchmark", str(args.runs)], check=True, env=run_env)


if __name__ == "__main__":
    main()
