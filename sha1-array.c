#include "cache.h"
#include "sha1-array.h"
#include "sha1-lookup.h"

void sha1_array_append(struct sha1_array *array, const unsigned char *sha1)
{
	ALLOC_GROW(array->oid, array->nr + 1, array->alloc);
	hashcpy(array->oid[array->nr++].hash, sha1);
	array->sorted = 0;
}

static int void_hashcmp(const void *a, const void *b)
{
	return hashcmp(a, b);
}

static void sha1_array_sort(struct sha1_array *array)
{
	qsort(array->oid, array->nr, sizeof(*array->oid), void_hashcmp);
	array->sorted = 1;
}

static const struct object_id *oid_access(size_t index, void *table)
{
	struct object_id *array = table;
	return array + index;
}

int oid_array_lookup(struct sha1_array *array, const struct object_id *oid)
{
	if (!array->sorted)
		sha1_array_sort(array);
	return oid_pos(oid, array->oid, array->nr, oid_access);
}

void sha1_array_clear(struct sha1_array *array)
{
	free(array->oid);
	array->oid = NULL;
	array->nr = 0;
	array->alloc = 0;
	array->sorted = 0;
}

void sha1_array_for_each_unique(struct sha1_array *array,
				for_each_sha1_fn fn,
				void *data)
{
	int i;

	if (!array->sorted)
		sha1_array_sort(array);

	for (i = 0; i < array->nr; i++) {
		if (i > 0 && !oidcmp(&array->oid[i], &array->oid[i-1]))
			continue;
		fn(array->oid[i].hash, data);
	}
}
