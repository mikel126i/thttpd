#include "first.h"

#include "array.h"
#include "buffer.h"
#include "settings.h"   /* BUFFER_MAX_REUSE_SIZE */

#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include <errno.h>
#include <assert.h>

__attribute_cold__
static void array_extend(array * const a) {
    a->size  += 16;
    a->data   = realloc(a->data,   sizeof(*a->data)   * a->size);
    force_assert(a->data);
    memset(a->data+a->used, 0, (a->size-a->used)*sizeof(*a->data));
}

array *array_init(void) {
	array *a;

	a = calloc(1, sizeof(*a));
	force_assert(a);
	array_extend(a);

	return a;
}

void array_free_data(array * const a) {
	data_unset ** const data = a->data;
	const uint32_t sz = a->size;
	for (uint32_t i = 0; i < sz; ++i) {
		if (data[i]) data[i]->fn->free(data[i]);
	}
	free(data);
}

void array_copy_array(array * const dst, const array * const src) {
	array_free_data(dst);
	if (0 == src->size) return;

	dst->used = src->used;
	dst->size = src->size;

	dst->data = calloc(src->size, sizeof(*src->data));
	force_assert(NULL != dst->data);
	for (uint32_t i = 0; i < src->used; ++i) {
		dst->data[i] = src->data[i]->fn->copy(src->data[i]);
	}
}

void array_free(array * const a) {
	if (!a) return;
	array_free_data(a);
	free(a);
}

void array_reset_data_strings(array * const a) {
	if (!a) return;

	data_string ** const data = (data_string **)a->data;
	const uint32_t used = a->used;
	a->used = 0;
	for (uint32_t i = 0; i < used; ++i) {
		data_string * const ds = data[i];
		/*force_assert(ds->type == TYPE_STRING);*/
		buffer * const k = &ds->key;
		buffer * const v = &ds->value;
		if (k->size > BUFFER_MAX_REUSE_SIZE) buffer_reset(k);
		if (v->size > BUFFER_MAX_REUSE_SIZE) buffer_reset(v);
	}
}

#if 0 /*(unused; see array_extract_element_klen())*/
data_unset *array_pop(array * const a) {
	data_unset *du;

	force_assert(a->used != 0);

	a->used --;
	du = a->data[a->used];
	a->data[a->used] = NULL;

	return du;
}
#endif

__attribute_pure__
static int array_caseless_compare(const char * const a, const char * const b, const size_t len) {
    for (size_t i = 0; i < len; ++i) {
        unsigned int ca = ((unsigned char *)a)[i];
        unsigned int cb = ((unsigned char *)b)[i];
        if (ca == cb) continue;

        /* always lowercase for transitive results */
        if (ca >= 'A' && ca <= 'Z') ca |= 32;
        if (cb >= 'A' && cb <= 'Z') cb |= 32;

        if (ca == cb) continue;
        return (int)(ca - cb);
    }
    return 0;
}

__attribute_pure__
static int array_keycmp(const char * const a, const size_t alen, const char * const b, const size_t blen) {
    return alen < blen ? -1 : alen > blen ? 1 : array_caseless_compare(a, b, blen);
}

/* If key is found, returns pos (pos >= 0) into a->data[]
 * If key is not found, returns -pos-1 if that is the position-1 in a->data[]
 * where the key would be inserted (-1 to avoid -0)
 */
__attribute_hot__
__attribute_pure__
static int32_t array_get_index(const array * const a, const char * const k, const size_t klen) {
    /* invariant: [lower-1] < probe < [upper]
     * invariant: 0 <= lower <= upper <= a->used
     */
    uint32_t lower = 0, upper = a->used;
    while (lower != upper) {
        uint32_t probe = (lower + upper) / 2;
        const buffer * const b = &a->data[probe]->key;
        /* key is non-empty (0==b->used), though possibly blank (1==b->used),
         * if inserted into key-value array */
        /*force_assert(b && b->used);*/
        int cmp = array_keycmp(k, klen, b->ptr, b->used-1);
        /*int cmp = array_keycmp(k, klen, CONST_BUF_LEN(b));*/
        if (cmp < 0)           /* key < [probe] */
            upper = probe;     /* still: lower <= upper */
        else if (cmp > 0)      /* key > [probe] */
            lower = probe + 1; /* still: lower <= upper */
        else  /*(cmp == 0)*/   /* found */
            return (int32_t)probe;
    }
    /* not found: [lower-1] < key < [upper] = [lower] ==> insert at [lower] */
    return -(int)lower - 1;
}

__attribute_hot__
data_unset *array_get_element_klen(const array * const a, const char *key, const size_t klen) {
    const int32_t ipos = array_get_index(a, key, klen);
    return ipos >= 0 ? a->data[ipos] : NULL;
}

/* non-const (data_config *) for configparser.y (not array_get_element_klen())*/
data_unset *array_get_data_unset(const array * const a, const char *key, const size_t klen) {
    const int32_t ipos = array_get_index(a, key, klen);
    return ipos >= 0 ? a->data[ipos] : NULL;
}

data_unset *array_extract_element_klen(array * const a, const char *key, const size_t klen) {
    const int32_t ipos = array_get_index(a, key, klen);
    if (ipos < 0) return NULL;

    /* remove entry from a->data: move everything after pos one step left */
    data_unset * const entry = a->data[ipos];
    const uint32_t last_ndx = --a->used;
    if (last_ndx != (uint32_t)ipos) {
        data_unset ** const d = a->data + ipos;
        memmove(d, d+1, (last_ndx - (uint32_t)ipos) * sizeof(*d));
    }
    a->data[last_ndx] = NULL;
    return entry;
}

static data_unset *array_get_unused_element(array * const a, const data_type_t t) {
    /* After initial startup and config, most array usage is of homogenous types
     * and arrays are cleared once per request, so check only the first unused
     * element to see if it can be reused */
  #if 1
    data_unset * const du = (a->used < a->size) ? a->data[a->used] : NULL;
    if (NULL != du && du->type == t) {
        a->data[a->used] = NULL;/* make empty slot at a->used for next insert */
        return du;
    }
    return NULL;
  #else
	data_unset ** const data = a->data;
	for (uint32_t i = a->used, sz = a->size; i < sz; ++i) {
		if (data[i] && data[i]->type == t) {
			data_unset * const ds = data[i];

			/* make empty slot at a->used for next insert */
			data[i] = data[a->used];
			data[a->used] = NULL;

			return ds;
		}
	}

	return NULL;
  #endif
}

static void array_insert_data_at_pos(array * const a, data_unset * const entry, const uint32_t pos) {
    /* This data structure should not be used for nearly so many entries */
    force_assert(a->used + 1 <= INT32_MAX);

    if (a->size == a->used) {
        array_extend(a);
    }

    const uint32_t ndx = a->used++;
    data_unset * const prev = a->data[ndx];

    /* move everything one step to the right */
    if (pos != ndx) {
        memmove(a->data + (pos + 1), a->data + (pos), (ndx - pos) * sizeof(*a->data));
    }
    a->data[pos] = entry;

    if (prev) prev->fn->free(prev); /* free prior data, if any, from slot */
}

static data_integer * array_insert_integer_at_pos(array * const a, const uint32_t pos) {
  #if 0 /*(not currently used by lighttpd in way that reuse would occur)*/
    data_integer *di = (data_integer *)array_get_unused_element(a,TYPE_INTEGER);
    if (NULL == di) di = data_integer_init();
  #else
    data_integer * const di = data_integer_init();
  #endif
    array_insert_data_at_pos(a, (data_unset *)di, pos);
    return di;
}

static data_string * array_insert_string_at_pos(array * const a, const uint32_t pos) {
    data_string *ds = (data_string *)array_get_unused_element(a, TYPE_STRING);
    if (NULL == ds) ds = data_string_init();
    array_insert_data_at_pos(a, (data_unset *)ds, pos);
    return ds;
}

int * array_get_int_ptr(array * const a, const char * const k, const size_t klen) {
    int32_t ipos = array_get_index(a, k, klen);
    if (ipos >= 0) return &((data_integer *)a->data[ipos])->value;

    data_integer * const di =array_insert_integer_at_pos(a,(uint32_t)(-ipos-1));
    buffer_copy_string_len(&di->key, k, klen);
    di->value = 0;
    return &di->value;
}

buffer * array_get_buf_ptr(array * const a, const char * const k, const size_t klen) {
    int32_t ipos = array_get_index(a, k, klen);
    if (ipos >= 0) return &((data_string *)a->data[ipos])->value;

    data_string * const ds = array_insert_string_at_pos(a, (uint32_t)(-ipos-1));
    buffer_copy_string_len(&ds->key, k, klen);
    buffer_clear(&ds->value);
    return &ds->value;
}

void array_insert_value(array * const a, const char * const v, const size_t vlen) {
    data_string * const ds = array_insert_string_at_pos(a, a->used);
    buffer_clear(&ds->key);
    buffer_copy_string_len(&ds->value, v, vlen);
}

/* if entry already exists return pointer to existing entry, otherwise insert entry and return NULL */
__attribute_cold__
static data_unset **array_find_or_insert(array * const a, data_unset * const entry) {
    force_assert(NULL != entry);

    /* push value onto end of array if there is no key */
    if (buffer_is_empty(&entry->key)) {
        array_insert_data_at_pos(a, entry, a->used);
        return NULL;
    }

    /* try to find the entry */
    const int32_t ipos = array_get_index(a, CONST_BUF_LEN(&entry->key));
    if (ipos >= 0) return &a->data[ipos];

    array_insert_data_at_pos(a, entry, (uint32_t)(-ipos - 1));
    return NULL;
}

/* replace or insert data (free existing entry) */
void array_replace(array * const a, data_unset * const entry) {
	data_unset **old;

	if (NULL != (old = array_find_or_insert(a, entry))) {
		force_assert(*old != entry);
		(*old)->fn->free(*old);
		*old = entry;
	}
}

void array_insert_unique(array * const a, data_unset * const entry) {
	data_unset **old;

	if (NULL != (old = array_find_or_insert(a, entry))) {
		force_assert((*old)->type == entry->type);
		entry->fn->insert_dup(*old, entry);
	}
}

int array_is_vlist(const array * const a) {
	for (uint32_t i = 0; i < a->used; ++i) {
		data_unset *du = a->data[i];
		if (!buffer_is_empty(&du->key) || du->type != TYPE_STRING) return 0;
	}
	return 1;
}

int array_is_kvany(const array * const a) {
	for (uint32_t i = 0; i < a->used; ++i) {
		data_unset *du = a->data[i];
		if (buffer_is_empty(&du->key)) return 0;
	}
	return 1;
}

int array_is_kvarray(const array * const a) {
	for (uint32_t i = 0; i < a->used; ++i) {
		data_unset *du = a->data[i];
		if (buffer_is_empty(&du->key) || du->type != TYPE_ARRAY) return 0;
	}
	return 1;
}

int array_is_kvstring(const array * const a) {
	for (uint32_t i = 0; i < a->used; ++i) {
		data_unset *du = a->data[i];
		if (buffer_is_empty(&du->key) || du->type != TYPE_STRING) return 0;
	}
	return 1;
}

/* array_match_*() routines follow very similar pattern, but operate on slightly
 * different data: array key/value, prefix/suffix match, case-insensitive or not
 * While these could be combined into fewer routines with flags to modify the
 * behavior, the interface distinctions are useful to add clarity to the code,
 * and the specialized routines run slightly faster */

data_unset *
array_match_key_prefix_klen (const array * const a, const char * const s, const size_t slen)
{
    for (uint32_t i = 0; i < a->used; ++i) {
        const buffer * const key = &a->data[i]->key;
        const size_t klen = buffer_string_length(key);
        if (klen <= slen && 0 == memcmp(s, key->ptr, klen))
            return a->data[i];
    }
    return NULL;
}

data_unset *
array_match_key_prefix_nc_klen (const array * const a, const char * const s, const size_t slen)
{
    for (uint32_t i = 0; i < a->used; ++i) {
        const buffer * const key = &a->data[i]->key;
        const size_t klen = buffer_string_length(key);
        if (klen <= slen && buffer_eq_icase_ssn(s, key->ptr, klen))
            return a->data[i];
    }
    return NULL;
}

data_unset *
array_match_key_prefix (const array * const a, const buffer * const b)
{
    return array_match_key_prefix_klen(a, CONST_BUF_LEN(b));
}

data_unset *
array_match_key_prefix_nc (const array * const a, const buffer * const b)
{
    return array_match_key_prefix_nc_klen(a, CONST_BUF_LEN(b));
}

const buffer *
array_match_value_prefix (const array * const a, const buffer * const b)
{
    const size_t blen = buffer_string_length(b);

    for (uint32_t i = 0; i < a->used; ++i) {
        const buffer * const value = &((data_string *)a->data[i])->value;
        const size_t vlen = buffer_string_length(value);
        if (vlen <= blen && 0 == memcmp(b->ptr, value->ptr, vlen))
            return value;
    }
    return NULL;
}

const buffer *
array_match_value_prefix_nc (const array * const a, const buffer * const b)
{
    const size_t blen = buffer_string_length(b);

    for (uint32_t i = 0; i < a->used; ++i) {
        const buffer * const value = &((data_string *)a->data[i])->value;
        const size_t vlen = buffer_string_length(value);
        if (vlen <= blen && buffer_eq_icase_ssn(b->ptr, value->ptr, vlen))
            return value;
    }
    return NULL;
}

data_unset *
array_match_key_suffix (const array * const a, const buffer * const b)
{
    const size_t blen = buffer_string_length(b);
    const char * const end = b->ptr + blen;

    for (uint32_t i = 0; i < a->used; ++i) {
        const buffer * const key = &a->data[i]->key;
        const size_t klen = buffer_string_length(key);
        if (klen <= blen && 0 == memcmp(end - klen, key->ptr, klen))
            return a->data[i];
    }
    return NULL;
}

data_unset *
array_match_key_suffix_nc (const array * const a, const buffer * const b)
{
    const size_t blen = buffer_string_length(b);
    const char * const end = b->ptr + blen;

    for (uint32_t i = 0; i < a->used; ++i) {
        const buffer * const key = &a->data[i]->key;
        const size_t klen = buffer_string_length(key);
        if (klen <= blen && buffer_eq_icase_ssn(end - klen, key->ptr, klen))
            return a->data[i];
    }
    return NULL;
}

const buffer *
array_match_value_suffix (const array * const a, const buffer * const b)
{
    const size_t blen = buffer_string_length(b);
    const char * const end = b->ptr + blen;

    for (uint32_t i = 0; i < a->used; ++i) {
        const buffer * const value = &((data_string *)a->data[i])->value;
        const size_t vlen = buffer_string_length(value);
        if (vlen <= blen && 0 == memcmp(end - vlen, value->ptr, vlen))
            return value;
    }
    return NULL;
}

const buffer *
array_match_value_suffix_nc (const array * const a, const buffer * const b)
{
    const size_t blen = buffer_string_length(b);
    const char * const end = b->ptr + blen;

    for (uint32_t i = 0; i < a->used; ++i) {
        const buffer * const value = &((data_string *)a->data[i])->value;
        const size_t vlen = buffer_string_length(value);
        if (vlen <= blen && buffer_eq_icase_ssn(end - vlen, value->ptr, vlen))
            return value;
    }
    return NULL;
}

data_unset *
array_match_path_or_ext (const array * const a, const buffer * const b)
{
    const size_t blen = buffer_string_length(b);

    for (uint32_t i = 0; i < a->used; ++i) {
        /* check extension in the form "^/path" or ".ext$" */
        const buffer * const key = &a->data[i]->key;
        const size_t klen = buffer_string_length(key);
        if (klen <= blen
            && 0 == memcmp((*(key->ptr) == '/' ? b->ptr : b->ptr + blen - klen),
                           key->ptr, klen))
            return a->data[i];
    }
    return NULL;
}





#include <stdio.h>

void array_print_indent(int depth) {
	int i;
	for (i = 0; i < depth; i ++) {
		fprintf(stdout, "    ");
	}
}

size_t array_get_max_key_length(const array * const a) {
	size_t maxlen = 0;
	for (uint32_t i = 0; i < a->used; ++i) {
		const buffer * const k = &a->data[i]->key;
		size_t len = buffer_string_length(k);

		if (len > maxlen) {
			maxlen = len;
		}
	}
	return maxlen;
}

int array_print(const array * const a, int depth) {
	uint32_t i;
	size_t maxlen;
	int oneline = 1;

	if (a->used > 5) {
		oneline = 0;
	}
	for (i = 0; i < a->used && oneline; i++) {
		data_unset *du = a->data[i];
		if (!buffer_is_empty(&du->key)) {
			oneline = 0;
			break;
		}
		switch (du->type) {
			case TYPE_INTEGER:
			case TYPE_STRING:
				break;
			default:
				oneline = 0;
				break;
		}
	}
	if (oneline) {
		fprintf(stdout, "(");
		for (i = 0; i < a->used; i++) {
			data_unset *du = a->data[i];
			if (i != 0) {
				fprintf(stdout, ", ");
			}
			du->fn->print(du, depth + 1);
		}
		fprintf(stdout, ")");
		return 0;
	}

	maxlen = array_get_max_key_length(a);
	fprintf(stdout, "(\n");
	for (i = 0; i < a->used; i++) {
		data_unset *du = a->data[i];
		array_print_indent(depth + 1);
		if (!buffer_is_empty(&du->key)) {
			int j;

			if (i && (i % 5) == 0) {
				fprintf(stdout, "# %u\n", i);
				array_print_indent(depth + 1);
			}
			fprintf(stdout, "\"%s\"", du->key.ptr);
			for (j = maxlen - buffer_string_length(&du->key); j > 0; j--) {
				fprintf(stdout, " ");
			}
			fprintf(stdout, " => ");
		}
		du->fn->print(du, depth + 1);
		fprintf(stdout, ",\n");
	}
	if (!(i && (i - 1 % 5) == 0)) {
		array_print_indent(depth + 1);
		fprintf(stdout, "# %u\n", i);
	}
	array_print_indent(depth);
	fprintf(stdout, ")");

	return 0;
}
