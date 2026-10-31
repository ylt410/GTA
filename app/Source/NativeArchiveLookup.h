#ifndef PG_NATIVE_ARCHIVE_LOOKUP_H
#define PG_NATIVE_ARCHIVE_LOOKUP_H

/* Portable lookup for replacing many archive entries in one DFS pass. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>

#ifndef PG_ARCHIVE_LOOKUP_COMPARE
#define PG_ARCHIVE_LOOKUP_COMPARE(a, b) strcasecmp((a), (b))
#endif

typedef struct PGArchiveLookupTarget {
	const char *path;
	size_t sourceIndex;
} PGArchiveLookupTarget;

/* Reject overlong paths before descent: snprintf's return is the untruncated
 * length, not a safe position in the destination buffer. */
static inline size_t pg_archive_path_append(char *path, size_t capacity,
                                          size_t length, const char *name) {
	if (length >= capacity) return SIZE_MAX;
	int added = snprintf(path + length, capacity - length, "%s%s", length ? "/" : "", name);
	if (added < 0 || (size_t)added >= capacity - length) {
		path[length] = 0;
		return SIZE_MAX;
	}
	return length + (size_t)added;
}

/* qsort order must use the same comparator as leaf matching. */
static inline int pg_archive_lookup_target_compare(const void *left, const void *right) {
	const PGArchiveLookupTarget *a = (const PGArchiveLookupTarget *)left;
	const PGArchiveLookupTarget *b = (const PGArchiveLookupTarget *)right;
	int order = PG_ARCHIVE_LOOKUP_COMPARE(a->path, b->path);
	if (order) return order;
	return a->sourceIndex < b->sourceIndex ? -1 : a->sourceIndex > b->sourceIndex;
}

static inline void pg_archive_lookup_sort(PGArchiveLookupTarget *targets, size_t count) {
	if (count > 1) qsort(targets, count, sizeof(*targets), pg_archive_lookup_target_compare);
}

/*
 * Report one DFS leaf to the sorted target set. The caller walks archive
 * entries exactly once in DFS order. Each request is filled only once, so a
 * duplicate archive path keeps the first leaf, matching pg_rpf_walk. Results
 * are indexed by sourceIndex and must be zero-initialized before the walk.
 * Returns the number of target requests matched by this leaf.
 */
static inline size_t pg_archive_lookup_match_leaf(const PGArchiveLookupTarget *targets,
										  size_t targetCount,
										  const char *leafPath,
										  uint64_t entryAddress,
										  uint64_t *results) {
	size_t low = 0, high = targetCount, matched = 0;
	while (low < high) {
		size_t middle = low + (high - low) / 2;
		if (PG_ARCHIVE_LOOKUP_COMPARE(targets[middle].path, leafPath) < 0) low = middle + 1;
		else high = middle;
	}
	for (size_t i = low; i < targetCount &&
		 PG_ARCHIVE_LOOKUP_COMPARE(targets[i].path, leafPath) == 0; ++i) {
		if (!results[targets[i].sourceIndex]) {
			results[targets[i].sourceIndex] = entryAddress;
			++matched;
		}
	}
	return matched;
}

#endif /* PG_NATIVE_ARCHIVE_LOOKUP_H */
