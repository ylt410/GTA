#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../Source/NativeArchiveLookup.h"

typedef struct Entry {
	const char *name;
	int directory;
	uint32_t firstChild;
	uint32_t childCount;
	uint64_t address;
} Entry;

static const Entry entries[] = {
	{"", 1, 1, 3, 0},
	{"Vehicles", 1, 4, 3, 0},
	{"Other", 1, 7, 1, 0},
	{"root-only.txt", 0, 0, 0, 0x3000},
	{"Oracle.yft", 0, 0, 0, 0x1000},
	{"ORACLE.YFT", 0, 0, 0, 0x2000},
	{"nested", 1, 8, 1, 0},
	{"oracle.yft", 0, 0, 0, 0x4000},
	{"Oracle.yft", 0, 0, 0, 0x5000},
};

static void join_path(char *path, size_t oldLength, const char *name) {
	size_t at = oldLength;
	if (at) path[at++] = '/';
	size_t n = strlen(name);
	memcpy(path + at, name, n + 1);
}

static void walk_old(uint32_t index, char *path, size_t length, const char *find, uint64_t *found) {
	const Entry *entry = &entries[index];
	size_t oldLength = length;
	if (index) {
		join_path(path, length, entry->name);
		length += (length ? 1 : 0) + strlen(entry->name);
	}
	if (entry->directory) {
		for (uint32_t i = 0; i < entry->childCount; ++i)
			walk_old(entry->firstChild + i, path, length, find, found);
	} else if (!*found && !strcasecmp(path, find)) {
		*found = entry->address;
	}
	path[oldLength] = 0;
}

static void walk_new(uint32_t index, char *path, size_t length,
					 const PGArchiveLookupTarget *targets, size_t count, uint64_t *results) {
	const Entry *entry = &entries[index];
	size_t oldLength = length;
	if (index) {
		join_path(path, length, entry->name);
		length += (length ? 1 : 0) + strlen(entry->name);
	}
	if (entry->directory) {
		for (uint32_t i = 0; i < entry->childCount; ++i)
			walk_new(entry->firstChild + i, path, length, targets, count, results);
	} else {
		(void)pg_archive_lookup_match_leaf(targets, count, path, entry->address, results);
	}
	path[oldLength] = 0;
}

int main(void) {
	static const char *const query[] = {
		"vehicles/oracle.yft", "VEHICLES/ORACLE.YFT", "other/oracle.yft",
		"vehicles/nested/oracle.yft", "vehicles/missing.yft", "root-only.txt",
	};
	static const uint64_t expected[] = {0x1000, 0x1000, 0x4000, 0x5000, 0, 0x3000};
	PGArchiveLookupTarget targets[sizeof(query) / sizeof(query[0])];
	uint64_t actual[sizeof(query) / sizeof(query[0])] = {0};
	char path[512] = {0};
	for (size_t i = 0; i < sizeof(query) / sizeof(query[0]); ++i) {
		uint64_t old = 0;
		walk_old(0, path, 0, query[i], &old);
		assert(old == expected[i]);
		targets[i].path = query[i];
		targets[i].sourceIndex = i;
	}
	pg_archive_lookup_sort(targets, sizeof(query) / sizeof(query[0]));
	walk_new(0, path, 0, targets, sizeof(query) / sizeof(query[0]), actual);
	for (size_t i = 0; i < sizeof(query) / sizeof(query[0]); ++i) {
		assert(actual[i] == expected[i]);
	}

	/* The helper compares full paths, including long names and POSIX case folding. */
	char longPath[402];
	memset(longPath, 'a', sizeof(longPath) - 1);
	longPath[sizeof(longPath) - 1] = 0;
	PGArchiveLookupTarget longTarget = {longPath, 0};
	uint64_t longResult[1] = {0};
	pg_archive_lookup_sort(&longTarget, 1);
	char upperPath[sizeof(longPath)];
	for (size_t i = 0; i < sizeof(longPath); ++i)
		upperPath[i] = longPath[i] >= 'a' && longPath[i] <= 'z' ? (char)(longPath[i] - 'a' + 'A') : longPath[i];
	assert(pg_archive_lookup_match_leaf(&longTarget, 1, upperPath, 0x9000, longResult) == 1);
	assert(longResult[0] == 0x9000);

	puts("archive lookup semantics passed");
	return 0;
}
