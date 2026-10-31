#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static uint64_t compareCount;
static int counted_strcasecmp(const char *a, const char *b);
#define PG_ARCHIVE_LOOKUP_COMPARE(a, b) counted_strcasecmp((a), (b))
#include "../Source/NativeArchiveLookup.h"

enum { LEAF_COUNT = 32768, TARGET_COUNT = 512, PATH_CAPACITY = 256, DUPLICATE_AT = 1234, RUN_COUNT = 7 };

typedef struct Leaf {
	char path[PATH_CAPACITY];
	uint64_t address;
} Leaf;

static int counted_strcasecmp(const char *a, const char *b) {
	++compareCount;
	return strcasecmp(a, b);
}

static uint64_t now_ns(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void make_path(char *out, size_t capacity, unsigned index) {
	(void)snprintf(out, capacity,
		"x64/dlc_packs/pack_%03u/levels/long_directory_component/vehicles/vehicle_%05u.yft",
		index % 173, index);
}

static void uppercase_ascii(char *text) {
	for (; *text; ++text)
		if (*text >= 'a' && *text <= 'z') *text = (char)(*text - 'a' + 'A');
}

static uint64_t run_old(const Leaf *leaves, size_t leafCount,
						const char *const *queries, size_t queryCount, uint64_t *results) {
	uint64_t start = now_ns();
	for (size_t q = 0; q < queryCount; ++q) {
		for (size_t i = 0; i < leafCount; ++i) {
			/* Production's legacy DFS continues through every leaf after a hit. */
			if (PG_ARCHIVE_LOOKUP_COMPARE(leaves[i].path, queries[q]) == 0 && !results[q]) {
				results[q] = leaves[i].address;
			}
		}
	}
	return now_ns() - start;
}

static uint64_t run_new(const Leaf *leaves, size_t leafCount,
						PGArchiveLookupTarget *targets, size_t targetCount,
						uint64_t *results) {
	uint64_t start = now_ns();
	pg_archive_lookup_sort(targets, targetCount);
	for (size_t i = 0; i < leafCount; ++i)
		(void)pg_archive_lookup_match_leaf(targets, targetCount, leaves[i].path, leaves[i].address, results);
	return now_ns() - start;
}

static uint64_t median(uint64_t samples[RUN_COUNT]) {
	for (size_t i = 0; i < RUN_COUNT; ++i)
		for (size_t j = i + 1; j < RUN_COUNT; ++j)
			if (samples[j] < samples[i]) { uint64_t t = samples[i]; samples[i] = samples[j]; samples[j] = t; }
	return samples[RUN_COUNT / 2];
}

int main(void) {
	const size_t leafCount = LEAF_COUNT + 1; /* one duplicate full path tests first-match semantics */
	size_t minPathBytes = (size_t)-1, maxPathBytes = 0;
	Leaf *leaves = (Leaf *)calloc(leafCount, sizeof(*leaves));
	char (*queryStorage)[PATH_CAPACITY] = (char (*)[PATH_CAPACITY])calloc(TARGET_COUNT, PATH_CAPACITY);
	const char **queries = (const char **)calloc(TARGET_COUNT, sizeof(*queries));
	PGArchiveLookupTarget *targets = (PGArchiveLookupTarget *)calloc(TARGET_COUNT, sizeof(*targets));
	uint64_t *oldResults = (uint64_t *)calloc(TARGET_COUNT, sizeof(*oldResults));
	uint64_t *newResults = (uint64_t *)calloc(TARGET_COUNT, sizeof(*newResults));
	if (!leaves || !queryStorage || !queries || !targets || !oldResults || !newResults) {
		fputs("allocation failed\n", stderr);
		free(leaves); free(queryStorage); free(queries); free(targets); free(oldResults); free(newResults);
		return 2;
	}

	size_t out = 0;
	for (unsigned i = 0; i < LEAF_COUNT; ++i) {
		make_path(leaves[out].path, sizeof(leaves[out].path), i);
		leaves[out++].address = (uint64_t)i + 1;
		if (i == DUPLICATE_AT) {
			make_path(leaves[out].path, sizeof(leaves[out].path), i);
			leaves[out++].address = UINT64_C(0xfeed0001);
		}
	}

	for (size_t i = 0; i < TARGET_COUNT; ++i) {
		if (i && i % 16 == 0) {
			strcpy(queryStorage[i], queryStorage[i - 1]); /* duplicate mod request */
		} else if (i % 13 == 0) {
			(void)snprintf(queryStorage[i], PATH_CAPACITY, "x64/missing/not_present_%04zu.yft", i);
		} else {
			unsigned leafIndex = i == 7 || i == 8 ? DUPLICATE_AT : (unsigned)((i * 7919u) % LEAF_COUNT);
			make_path(queryStorage[i], PATH_CAPACITY, leafIndex);
		}
		if (i % 3 == 0) uppercase_ascii(queryStorage[i]); /* case-insensitive matching */
		queries[i] = queryStorage[i];
		targets[i].path = queryStorage[i];
		targets[i].sourceIndex = i;
		size_t pathBytes = strlen(queryStorage[i]);
		if (pathBytes < minPathBytes) minPathBytes = pathBytes;
		if (pathBytes > maxPathBytes) maxPathBytes = pathBytes;
	}

	uint64_t oldSamples[RUN_COUNT], newSamples[RUN_COUNT];
	uint64_t oldComparisons = 0, newComparisons = 0;
	for (size_t run = 0; run < RUN_COUNT; ++run) {
		for (size_t order = 0; order < 2; ++order) {
			const int runNewFirst = (run & 1) != 0;
			const int useNew = runNewFirst ? order == 0 : order == 1;
			if (useNew) {
				memset(newResults, 0, TARGET_COUNT * sizeof(*newResults));
				for (size_t i = 0; i < TARGET_COUNT; ++i) targets[i] = (PGArchiveLookupTarget){queries[i], i};
				compareCount = 0;
				newSamples[run] = run_new(leaves, leafCount, targets, TARGET_COUNT, newResults);
				newComparisons = compareCount;
			} else {
				memset(oldResults, 0, TARGET_COUNT * sizeof(*oldResults));
				compareCount = 0;
				oldSamples[run] = run_old(leaves, leafCount, queries, TARGET_COUNT, oldResults);
				oldComparisons = compareCount;
			}
		}
		for (size_t i = 0; i < TARGET_COUNT; ++i) {
			if (oldResults[i] != newResults[i]) {
				fprintf(stderr, "result mismatch for request %zu in run %zu: old=%" PRIu64 " new=%" PRIu64 "\n",
					i, run, oldResults[i], newResults[i]);
				return 1;
			}
		}
	}
	uint64_t oldNs = median(oldSamples), newNs = median(newSamples);
	if (newResults[7] != DUPLICATE_AT + 1 || newResults[8] != DUPLICATE_AT + 1) {
		fputs("duplicate-leaf first-match check failed\n", stderr);
		return 1;
	}

	printf("{\"leaves\":%zu,\"targets\":%d,\"runs\":%d,\"min_target_path_bytes\":%zu,\"max_target_path_bytes\":%zu,\"old_comparisons_per_run\":%" PRIu64
		",\"new_comparisons_per_run\":%" PRIu64 ",\"old_ns_median\":%" PRIu64 ",\"new_ns_median\":%" PRIu64
		",\"results_match\":true}\n",
		leafCount, TARGET_COUNT, RUN_COUNT, minPathBytes, maxPathBytes, oldComparisons, newComparisons, oldNs, newNs);

	free(leaves); free(queryStorage); free(queries); free(targets); free(oldResults); free(newResults);
	return 0;
}
