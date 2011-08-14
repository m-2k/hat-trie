/*
 * This file is part of hat-trie.
 *
 * Copyright (c) 2011 by Daniel C. Jones <dcjones@cs.washington.edu>
 *
 */

#include "ahtable.h"
#include "misc.h"
#include "superfasthash.h"
#include "config.h"
#include <assert.h>
#include <string.h>


const double ahtable_max_load_factor = 5.0;
const const size_t ahtable_initial_size = 8;
static const uint16_t LONG_KEYLEN_MASK = 0x7fff;


ahtable_t* ahtable_create()
{
    return ahtable_create_n(ahtable_initial_size);
}


ahtable_t* ahtable_create_n(size_t n)
{
    ahtable_t* T = malloc_or_die(sizeof(ahtable_t));
    T->flag = 0;
    T->c0 = T->c1 = '\0';

    T->n = n;
    T->m = 0;
    T->max_m = (size_t) (ahtable_max_load_factor * (double) T->n);
    T->slots = malloc_or_die(n * sizeof(slot_t));
    memset(T->slots, 0, n * sizeof(slot_t));
    return T;
}


void ahtable_free(ahtable_t* T)
{
    size_t i;
    for (i = 0; i < T->n; ++i) free(T->slots[i]);
    free(T->slots);
    free(T);
}


ahtable_t* ahtable_dup(const ahtable_t* T)
{
    ahtable_t* S = ahtable_create_n(T->n);
    memcpy(S->slots, T->slots, T->n * sizeof(slot_t));
    S->flag   = T->flag;
    S->c0     = T->c0;
    S->c1     = T->c1;
    S->m      = T->m;
    return S;
}


size_t ahtable_size(const ahtable_t* T)
{
    return T->m;
}


void ahtable_clear(ahtable_t* T)
{
    size_t i;
    for (i = 0; i < T->n; ++i) free(T->slots[i]);
    T->n = ahtable_initial_size;
    T->slots = realloc_or_die(T->slots, T->n * sizeof(slot_t));
    memset(T->slots, 0, T->n * sizeof(slot_t));
}


static slot_t ins_key(slot_t s, const char* key, size_t len, value_t** val)
{
    // key length
    if (len < 128) {
        s[0] = (unsigned char) len;
        s += 1;
    }
    else {
        /* The most significant bit is set to indicate that two bytes are
         * being used to store the key length. */
        *((uint16_t*) s) = (uint16_t) len | 0x8000;
        s += 2;
    }

    // key
    memcpy(s, key, len * sizeof(unsigned char));
    s += len;

    // value
    *val = (value_t*) s;
    **val = 0;
    s += sizeof(value_t);

    return s;
}


static void ahtable_expand(ahtable_t* T)
{
    /* Resizing a table is essentially building a brand new one.
     * One little shortcut we can take on the memory allocation front is to
     * figure out how much memory each slot needs in advance.
     */
    size_t new_n = 2 * T->n;
    size_t* slot_sizes = malloc_or_die(new_n * sizeof(size_t));
    memset(slot_sizes, 0, new_n * sizeof(size_t));

    const char* key;
    size_t len;
    ahtable_iter_t* i = ahtable_iter_begin(T);
    while (!ahtable_iter_finished(i)) {
        key = ahtable_iter_key(i, &len);
        slot_sizes[hash(key, len) % new_n] +=
            len + sizeof(value_t) + (len >= 128 ? 2 : 1);

        ahtable_iter_next(i);
    }
    ahtable_iter_free(i);


    /* allocate slots */
    slot_t* slots = malloc_or_die(new_n * sizeof(slot_t));
    size_t j;
    for (j = 0; j < new_n; ++j) {
        if (slot_sizes[j] > 0) {
            slots[j] = malloc_or_die(slot_sizes[j] + 1);
            slots[j][slot_sizes[j]] = '\0';
        }
        else slots[j] = NULL;
    }

    /* rehash values. A few shortcuts can be taken here as well, as we know
     * there will be no collisions. Instead of the regular insertion routine,
     * we keep track of the ends of every slot and simply insert keys.
     * */
    slot_t* slots_next = malloc_or_die(new_n * sizeof(slot_t));
    memcpy(slots_next, slots, new_n * sizeof(slot_t));
    size_t h;
    size_t m = 0;
    value_t* u;
    value_t* v;
    i = ahtable_iter_begin(T);
    while (!ahtable_iter_finished(i)) {

        key = ahtable_iter_key(i, &len);
        h = hash(key, len) % new_n;

        slots_next[h] = ins_key(slots_next[h], key, len, &u);
        v = ahtable_iter_val(i);
        *u = *v;

        ++m;
        ahtable_iter_next(i);
    }
    ahtable_iter_free(i);

    assert(m == T->m);

    free(slots_next);
    for (j = 0; j < T->n; ++j) free(T->slots[j]);
    free(T->slots);
    T->slots = slots;
    T->n = new_n;
    T->max_m = (size_t) (ahtable_max_load_factor * (double) T->n);
    free(slot_sizes);
}


static value_t* get_key(ahtable_t* T, const char* key, size_t len, bool insert_missing)
{
    /* if we are at capacity, preemptively resize */
    if (insert_missing && T->m >= T->max_m) {
        ahtable_expand(T);
    }


    uint32_t i = hash(key, len) % T->n;
    size_t k;
    slot_t s;
    value_t* val;


    /* the easy case: empty slot */
    if (T->slots[i] == NULL) {
        if (insert_missing) {
            size_t slot_size = 0;
            slot_size += 1 + (len >= 128 ? 1 : 0);     // key length
            slot_size += len * sizeof(unsigned char); // key
            slot_size += sizeof(value_t);             // value
            slot_size += 1;                           // null-terminator
            s = T->slots[i] = malloc_or_die(slot_size);

            ++T->m;
            s = ins_key(s, key, len, &val);
            *s = '\0';

            return val;
        }
        else return NULL;
    }

    /* search the array for our key */
    s = T->slots[i];
    while (*s != '\0') {
        /* get the key length */
        if (0x80 & *s) {
            k = (size_t) (LONG_KEYLEN_MASK & *((uint16_t*) s));
            s += 2;
        }
        else {
            k = (size_t) *s;
            s += 1;
        }

        /* skip keys that are longer than ours */
        if (k != len) {
            s += k + sizeof(value_t);
            continue;
        }

        /* key found. */
        if (memcmp(s, key, len) == 0) {
            return (value_t*) (s + len);
        }
        /* key not found. */
        else {
            s += k + sizeof(value_t);
            continue;
        }
    }


    if (insert_missing) {
        /* the key was not found, so we must insert it. */
        size_t old_size = s - T->slots[i] + 1;
        size_t new_size = old_size;
        new_size += 1 + (len >= 128 ? 1 : 0);     // key length
        new_size += len * sizeof(unsigned char); // key
        new_size += sizeof(value_t);             // value

        s = T->slots[i] = realloc_or_die(T->slots[i], new_size);
        s[new_size - 1] = '\0';

        ++T->m;
        ins_key(s + old_size - 1, key, len, &val);

        return val;
    }
    else return NULL;
}


value_t* ahtable_get (ahtable_t* T, const char* key, size_t len)
{
    return get_key(T, key, len, true);
}


value_t* ahtable_tryget (ahtable_t* T, const char* key, size_t len )
{
    return get_key(T, key, len, false);
}



struct ahtable_iter_t_
{
    const ahtable_t* T; // parent
    size_t i;           // slot index
    slot_t s;           // slot position
};



ahtable_iter_t* ahtable_iter_begin(const ahtable_t* T)
{
    ahtable_iter_t* i = malloc_or_die(sizeof(ahtable_iter_t));
    i->T = T;

    for (i->i = 0; i->i < i->T->n; ++i->i) {
        i->s = T->slots[i->i];
        if (i->s == NULL) continue;
        if (*i->s == '\0') continue;
        break;
    }

    return i;
}


void ahtable_iter_next(ahtable_iter_t* i)
{
    if (ahtable_iter_finished(i)) return;

    size_t k;

    /* get the key length */
    if (0x80 & *i->s) {
        k = (size_t) (LONG_KEYLEN_MASK & *((uint16_t*) i->s));
        i->s += 2;
    }
    else {
        k = (size_t) *i->s;
        i->s += 1;
    }

    /* skip to the next key */
    i->s += k + sizeof(value_t);

    if (*i->s == '\0') {
        do {
            ++i->i;
        } while(i->i < i->T->n &&
                (i->T->slots[i->i] == NULL || *i->T->slots[i->i] == '\0'));

        if (i->i < i->T->n) i->s = i->T->slots[i->i];
    }
}



bool ahtable_iter_finished(ahtable_iter_t* i)
{
    return i->i >= i->T->n;
}


void ahtable_iter_free(ahtable_iter_t* i)
{
    free(i);
}



const char* ahtable_iter_key(ahtable_iter_t* i, size_t* len)
{
    if (ahtable_iter_finished(i)) return NULL;

    slot_t s = i->s;
    size_t k;
    if (0x80 & *s) {
        k = (size_t) (LONG_KEYLEN_MASK & *((uint16_t*) s));
        s += 2;
    }
    else {
        k = (size_t) *s;
        s += 1;
    }

    *len = k;
    return (const char*) s;
}


value_t* ahtable_iter_val(ahtable_iter_t* i)
{
    if (ahtable_iter_finished(i)) return NULL;

    slot_t s = i->s;

    size_t k;
    if (0x80 & *s) {
        k = (size_t) (LONG_KEYLEN_MASK & *((uint16_t*) s));
        s += 2;
    }
    else {
        k = (size_t) *s;
        s += 1;
    }

    s += k;
    return (value_t*) s;
}



