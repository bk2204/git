#include "cache.h"
#include "sha1-array.h"
#include "sha1-lookup.h"

void oid_array_append(struct oid_array *array, const struct object_id *oid)
{
	ALLOC_GROW(array->oid, array->nr + 1, array->alloc);
	oidcpy(&array->oid[array->nr++], oid);
	array->sorted = 0;
}

static int void_hashcmp(const void *a, const void *b)
{
	return hashcmp(a, b);
}

static void oid_array_sort(struct oid_array *array)
{
	qsort(array->oid, array->nr, sizeof(*array->oid), void_hashcmp);
	array->sorted = 1;
}

static const struct object_id *oid_access(size_t index, void *table)
{
	struct object_id *array = table;
	return array + index;
}

int oid_array_lookup(struct oid_array *array, const struct object_id *oid)
{
	if (!array->sorted)
		oid_array_sort(array);
	return oid_pos(oid, array->oid, array->nr, oid_access);
}

void oid_array_clear(struct oid_array *array)
{
	free(array->oid);
	array->oid = NULL;
	array->nr = 0;
	array->alloc = 0;
	array->sorted = 0;
}

void oid_array_for_each_unique(struct oid_array *array,
				for_each_oid_fn fn,
				void *data)
{
	int i;

	if (!array->sorted)
		oid_array_sort(array);

	for (i = 0; i < array->nr; i++) {
		if (i > 0 && !oidcmp(&array->oid[i], &array->oid[i-1]))
			continue;
		fn(array->oid + i, data);
	}
}
