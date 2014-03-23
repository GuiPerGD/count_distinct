/*
* count_distinct.c - alternative to COUNT(DISTINCT ...)
* Copyright (C) Tomas Vondra, 2013
*
*/

#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <limits.h>

#include "postgres.h"
#include "utils/datum.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
#include "nodes/execnodes.h"
#include "access/tupmacs.h"
#include "utils/pg_crc.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/* if set to 1, the table resize will be profiled */
#define DEBUG_PROFILE       0
#define DEBUG_HISTOGRAM     0   /* prints bucket size histogram */

#if (PG_VERSION_NUM >= 90000)

#define GET_AGG_CONTEXT(fname, fcinfo, aggcontext)  \
    if (! AggCheckCallContext(fcinfo, &aggcontext)) {   \
        elog(ERROR, "%s called in non-aggregate context", fname);  \
    }

#define CHECK_AGG_CONTEXT(fname, fcinfo)  \
    if (! AggCheckCallContext(fcinfo, NULL)) {   \
        elog(ERROR, "%s called in non-aggregate context", fname);  \
    }
    
#elif (PG_VERSION_NUM >= 80400)

#define GET_AGG_CONTEXT(fname, fcinfo, aggcontext)  \
    if (fcinfo->context && IsA(fcinfo->context, AggState)) {  \
        aggcontext = ((AggState *) fcinfo->context)->aggcontext;  \
    } else if (fcinfo->context && IsA(fcinfo->context, WindowAggState)) {  \
        aggcontext = ((WindowAggState *) fcinfo->context)->wincontext;  \
    } else {  \
        elog(ERROR, "%s called in non-aggregate context", fname);  \
        aggcontext = NULL;  \
    }

#define CHECK_AGG_CONTEXT(fname, fcinfo)  \
    if (!(fcinfo->context &&  \
        (IsA(fcinfo->context, AggState) ||  \
        IsA(fcinfo->context, WindowAggState))))  \
    {  \
        elog(ERROR, "%s called in non-aggregate context", fname);  \
    }
    
#else

#define GET_AGG_CONTEXT(fname, fcinfo, aggcontext)  \
    if (fcinfo->context && IsA(fcinfo->context, AggState)) {  \
        aggcontext = ((AggState *) fcinfo->context)->aggcontext;  \
    } else {  \
        elog(ERROR, "%s called in non-aggregate context", fname);  \
        aggcontext = NULL;  \
    }

#define CHECK_AGG_CONTEXT(fname, fcinfo)  \
    if (!(fcinfo->context &&  \
        (IsA(fcinfo->context, AggState))))  \
    {  \
        elog(ERROR, "%s called in non-aggregate context", fname);  \
    }

/* backward compatibility with 8.3 (macros copied mostly from src/include/access/tupmacs.h) */

#if SIZEOF_DATUM == 8

#define fetch_att(T,attbyval,attlen) \
( \
    (attbyval) ? \
    ( \
        (attlen) == (int) sizeof(Datum) ? \
            *((Datum *)(T)) \
        : \
      ( \
        (attlen) == (int) sizeof(int32) ? \
            Int32GetDatum(*((int32 *)(T))) \
        : \
        ( \
            (attlen) == (int) sizeof(int16) ? \
                Int16GetDatum(*((int16 *)(T))) \
            : \
            ( \
                AssertMacro((attlen) == 1), \
                CharGetDatum(*((char *)(T))) \
            ) \
        ) \
      ) \
    ) \
    : \
    PointerGetDatum((char *) (T)) \
)
#else                           /* SIZEOF_DATUM != 8 */

#define fetch_att(T,attbyval,attlen) \
( \
    (attbyval) ? \
    ( \
        (attlen) == (int) sizeof(int32) ? \
            Int32GetDatum(*((int32 *)(T))) \
        : \
        ( \
            (attlen) == (int) sizeof(int16) ? \
                Int16GetDatum(*((int16 *)(T))) \
            : \
            ( \
                AssertMacro((attlen) == 1), \
                CharGetDatum(*((char *)(T))) \
            ) \
        ) \
    ) \
    : \
    PointerGetDatum((char *) (T)) \
)
#endif   /* SIZEOF_DATUM == 8 */

#define att_addlength_pointer(cur_offset, attlen, attptr) \
( \
    ((attlen) > 0) ? \
    ( \
        (cur_offset) + (attlen) \
    ) \
    : (((attlen) == -1) ? \
    ( \
        (cur_offset) + VARSIZE_ANY(attptr) \
    ) \
    : \
    ( \
        AssertMacro((attlen) == -2), \
        (cur_offset) + (strlen((char *) (attptr)) + 1) \
    )) \
)

#define att_align_nominal(cur_offset, attalign) \
( \
    ((attalign) == 'i') ? INTALIGN(cur_offset) : \
     (((attalign) == 'c') ? (long) (cur_offset) : \
      (((attalign) == 'd') ? DOUBLEALIGN(cur_offset) : \
       ( \
            AssertMacro((attalign) == 's'), \
            SHORTALIGN(cur_offset) \
       ))) \
)
    
#endif

#define COMPUTE_CRC32(hash, value, length) \
    INIT_CRC32(hash); \
    COMP_CRC32(hash, value, length); \
    FIN_CRC32(hash);

/* hash table parameters */
#define HTAB_INIT_BITS      2      /* initial number of significant bits */
#define HTAB_INIT_SIZE      4      /* initial hash table size is only 4 buckets (80 items) */
#define HTAB_MAX_SIZE       262144 /* maximal hash table size is 256k buckets */
#define HTAB_BUCKET_LIMIT   20     /* when to resize the table (average bucket size limit) */

#define HTAB_BUCKET_STEP    5       /* bucket growth step (number of elements, not bytes) */

/* Structures used to keep the data - bucket and hash table. */

/* A single value in the hash table, along with it's 32-bit hash (so that we
 * don't need to compute it over and over).
 * 
 * The value is stored inline - for example to store int32 (4B) value, the palloc
 * would look like this
 * 
 *     palloc(offsetof(hash_element_t, value) + sizeof(int32))
 * 
 * and similarly for other data types. The important thing is that the structure
 * needs to be fixed length so that buckets can contain an array of items. So for
 * varlena types, there needs to be a pointer (either 4B or 8B) with value stored
 * somewhere else.
 * 
 * See HASH_ELEMENT_SIZE/GET_ELEMENT for evaluation of the element size and
 * accessing a particular item in a bucket.
 * 
 * TODO Is it really efficient to keep the hash, or should we save a bit of memory
 * and recompute the hash every time?
 */
typedef struct hash_element_t {
    
    uint32  hash;      /* 32-bit hash of this particular element */
    char    value[1];  /* the value itself (trick: fixed-length will be in-place) */
    
} hash_element_t;

/* A single bucket of the hash table - basically a simple list of items implemented
 * as an array (+length). This grows in steps (HTAB_BUCKET_STEP).
 */
typedef struct hash_bucket_t {
    
    uint32  nitems; /* items in this particular bucket */
    hash_element_t * items;   /* array of bucket elements (see GET_ELEMENT) */
    
} hash_bucket_t;

/* A hash table - a collection of buckets. */
typedef struct hash_table_t {
    
    uint16  length;     /* length of the value (depends on the actual data type) */
    uint16  nbits;      /* number of significant bits of the hash (HTAB_INIT_BITS by default) */
    uint32  nbuckets;   /* number of buckets (HTAB_INIT_SIZE), basically 2^nbits */
    uint32  nitems;     /* current number of elements of the hash table */
    
    hash_bucket_t *  buckets;
    
} hash_table_t;

#define HASH_ELEMENT_SIZE(htab)     (htab->length + offsetof(hash_element_t, value))
#define GET_ELEMENT(htab, bucket, item) \
    (hash_element_t*) ((char*) htab->buckets[bucket].items + (item * HASH_ELEMENT_SIZE(htab)))
#define GET_BUCKET_ELEMENT(htab, bucket, item) \
    (hash_element_t*) ((char*) bucket.items + (item * HASH_ELEMENT_SIZE(htab)))

/* prototypes */
PG_FUNCTION_INFO_V1(count_distinct_append);
PG_FUNCTION_INFO_V1(count_distinct);

Datum count_distinct_append(PG_FUNCTION_ARGS);
Datum count_distinct(PG_FUNCTION_ARGS);

static bool add_element_to_table(hash_table_t * htab, char * value);
static bool element_exists_in_bucket(hash_table_t * htab, uint32 hash, char * value, uint32 bucket);
static void resize_hash_table(hash_table_t * htab);
static hash_table_t * init_hash_table(int length);

#if DEBUG_PROFILE
static void print_table_stats(hash_table_t * htab);
#endif

Datum
count_distinct_append(PG_FUNCTION_ARGS)
{

    hash_table_t  *htab;

    /* info for anyelement */
    Oid         element_type = get_fn_expr_argtype(fcinfo->flinfo, 1);
    Datum       element = PG_GETARG_DATUM(1);
    int16       typlen;
    bool        typbyval;
    char        typalign;

    /* memory contexts */
    MemoryContext oldcontext;
    MemoryContext aggcontext;

    /* OK, we do want to skip NULL values altogether */
    if (PG_ARGISNULL(1)) {
        if (PG_ARGISNULL(0))
            PG_RETURN_NULL();   /* no state, no value -> just keep NULL */
        else
            /* if there already is a state accumulated, don't forget it */
            PG_RETURN_DATUM(PG_GETARG_DATUM(0));
    }

    /* we can be sure the value is not null (see the check above) */

    /* get type information for the second parameter (anyelement item) */
    get_typlenbyvalalign(element_type, &typlen, &typbyval, &typalign);

    /* we can't handle varlena types yet or values passed by reference */
    if ((typlen == -1) || (! typbyval))
        elog(ERROR, "count_distinct handles only fixed-length types passed by value");

    /* switch to the per-group hash-table memory context */
    GET_AGG_CONTEXT("count_distinct_append", fcinfo, aggcontext);

    oldcontext = MemoryContextSwitchTo(aggcontext);

    /* init the hash table, if needed */
    if (PG_ARGISNULL(0)) {
        htab = init_hash_table(typlen);
    } else {
        htab = (hash_table_t *)PG_GETARG_POINTER(0);
    }

    /* TODO The requests for type info shouldn't be a problem (thanks to lsyscache),
     * but if it turns out to have a noticeable impact it's possible to cache that
     * between the calls (in the estimator). */

    /* add the value into the hash table, check if we need to resize the table */
    add_element_to_table(htab, (char*)&element);
    
    if ((htab->nitems / htab->nbuckets >= HTAB_BUCKET_LIMIT) && (htab->nbuckets*4 <= HTAB_MAX_SIZE)) {
        /* do we need to increase the hash table size? only if we have too many elements in a bucket
         * (on average) and the table is not too large already */
        resize_hash_table(htab);
    }
    
    MemoryContextSwitchTo(oldcontext);
    
    PG_RETURN_POINTER(htab);

}

Datum
count_distinct(PG_FUNCTION_ARGS)
{
    
    hash_table_t * htab;
    
    CHECK_AGG_CONTEXT("count_distinct", fcinfo);
    
    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    
    htab = (hash_table_t *)PG_GETARG_POINTER(0);

#if DEBUG_PROFILE
    print_table_stats(htab);
#endif
    
    PG_RETURN_INT64(htab->nitems);

}

static
bool add_element_to_table(hash_table_t * htab, char * value) {
    
    uint32 hash;
    uint32 bucket;

    hash_element_t * element;
    
    /* compute the hash and keep only the first 4 bytes */
    COMPUTE_CRC32(hash, value, htab->length);

    /* get the bucket and then add the element to the bucket */
    bucket = ((1 << htab->nbits) - 1) & hash;
    
    /* not it's not, so let's add it to the hash table */
    if (! element_exists_in_bucket(htab, hash, value, bucket)) {
    
        /* if there's no space in the bucket, resize it */
        if (htab->buckets[bucket].nitems == 0) {
            htab->buckets[bucket].items = palloc(HTAB_BUCKET_STEP * HASH_ELEMENT_SIZE(htab));
        } else if (htab->buckets[bucket].nitems % HTAB_BUCKET_STEP == 0) {
            htab->buckets[bucket].items = repalloc(htab->buckets[bucket].items,
                                                (htab->buckets[bucket].nitems + HTAB_BUCKET_STEP) * HASH_ELEMENT_SIZE(htab));
        }
        
        /* get the element position right (needs to handle dynamic value lengths) */
        element = GET_ELEMENT(htab, bucket, htab->buckets[bucket].nitems);
        
        element->hash = hash;
        memcpy(&element->value, value, htab->length);
        
        htab->buckets[bucket].nitems += 1;
        htab->nitems += 1;
        
        return TRUE;
    
    }
    
    return FALSE;

}

static
bool element_exists_in_bucket(hash_table_t * htab, uint32 hash, char * value, uint32 bucket) {
    
    int i;
    hash_element_t * element;
    
    /* is the element already in the bucket? */
    for (i = 0; i < htab->buckets[bucket].nitems; i++) {

        /* get the element position right (needs to handle dynamic value lengths) */
        element = GET_ELEMENT(htab, bucket, i);

        if (element->hash == hash) {
            if (memcmp(element->value, value, htab->length) == 0) {
                return TRUE;
            }
        }
    }
    
    return FALSE;
    
}

static
hash_table_t * init_hash_table(int length) {
    
    hash_table_t * htab = (hash_table_t *)palloc(sizeof(hash_table_t));
    
    htab->length = length;
    htab->nbits = HTAB_INIT_BITS;
    htab->nbuckets = HTAB_INIT_SIZE;
    htab->nitems = 0;
        
    /* the memory is zeroed */
    htab->buckets = (hash_bucket_t *)palloc0(sizeof(hash_bucket_t) * HTAB_INIT_SIZE);
    
    return htab;
    
}

static
void resize_hash_table(hash_table_t * htab) {
    
    int i, j;
    hash_bucket_t old_bucket;
    
#if DEBUG_PROFILE
    struct timeval start_time, end_time;
    
    print_table_stats(htab);
    gettimeofday(&start_time, NULL);
#endif
    
    /* basic sanity checks */
    assert(htab != NULL); 
    assert((htab->nbuckets >= HTAB_INIT_SIZE) && (htab->nbuckets*4 <= HTAB_MAX_SIZE)); /* valid number of buckets */
    
    /* double the hash table size */
    htab->nbits += 2;
    
    htab->nitems = 0; /* we'll essentially re-add all the elements, which will set this back */
    htab->buckets = repalloc(htab->buckets, 4 * htab->nbuckets * sizeof(hash_bucket_t));
    
    /* but zero the new buckets, just to be sure (the size is in bytes) */
    memset(htab->buckets + htab->nbuckets, 0, 3*htab->nbuckets * sizeof(hash_bucket_t));
    
    /* now let's loop through the old buckets and re-add all the elements */
    for (i = 0; i < htab->nbuckets; i++) {

        if (htab->buckets[i].items == NULL) {
            continue;
        }
        
        /* keep the old values */
        old_bucket = htab->buckets[i];
        
        /* reset the bucket */
        htab->buckets[i].nitems = 0;
        htab->buckets[i].items  = NULL;
        
        for (j = 0; j < old_bucket.nitems; j++) {
            hash_element_t * element = GET_BUCKET_ELEMENT(htab, old_bucket, j);
            add_element_to_table(htab, element->value);
        }
        
        /* and finally release the old bucket */
        pfree(old_bucket.items);
        
    }
    
    /* finally, let's update the number of buckets */
    htab->nbuckets *= 4;
    
#if DEBUG_PROFILE

    gettimeofday(&end_time, NULL);
    print_table_stats(htab);
    
    elog(WARNING, "RESIZE: items=%d [%d => %d] duration=%ld us",
                htab->nitems, htab->nbuckets/4, htab->nbuckets,
                (end_time.tv_sec - start_time.tv_sec)*1000000 + (end_time.tv_usec - start_time.tv_usec));
    
#endif
    
}

#if DEBUG_PROFILE
static 
void print_table_stats(hash_table_t * htab) {
    
    int i;
    int32 * buckets;
    int min_items, max_items;
    double average, variance = 0;
    
    min_items = htab->nitems;
    max_items = 0;
    
    for (i = 0; i < htab->nbuckets; i++) {
        min_items = (htab->buckets[i].nitems < min_items) ? htab->buckets[i].nitems : min_items;
        max_items = (htab->buckets[i].nitems > max_items) ? htab->buckets[i].nitems : max_items;
    }
    
    elog(WARNING, "===== hash table stats =====");
    elog(WARNING, " items: %d", htab->nitems);
    elog(WARNING, " buckets: %d", htab->nbuckets);
    elog(WARNING, " min bucket size: %d", min_items);
    elog(WARNING, " max bucket size: %d", max_items);
    
    buckets = palloc0((max_items+1)*sizeof(int32));
    
    /* average number of items per bucket */
    average = (htab->nitems * 1.0) / htab->nbuckets;
    
    /* compute number of buckets for each bucket size in [0, max_items] */
    for (i = 0; i < htab->nbuckets; i++) {
        buckets[htab->buckets[i].nitems]++;
        variance += (htab->buckets[i].nitems - average) * (htab->buckets[i].nitems - average);
    }
    
    elog(WARNING, " bucket size variance: %.3f", variance/htab->nbuckets);
    elog(WARNING, " bucket size stddev: %.3f", sqrt(variance/htab->nbuckets));
    
#if DEBUG_HISTOGRAM
    
    /* now print the histogram (if enabled) */
    elog(WARNING, "--------- histogram ---------");
    
    for (i = 0; i <= max_items; i++) {
        elog(WARNING, "[%3d] => %7.3f%% [%d]", i, (buckets[i] * 100.0) / (htab->nbuckets), buckets[i]);
    }
    
#endif
    
    elog(WARNING, "============================");
    
    pfree(buckets);
    
}
#endif