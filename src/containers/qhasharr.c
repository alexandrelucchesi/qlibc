/******************************************************************************
 * qLibc
 *
 * Copyright (c) 2010-2015 Seungyoung Kim.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *****************************************************************************/

/**
 * @file qhasharr.c Static(array) hash-table implementation.
 *
 * qhasharr implements a hash-table which maps keys to values and stores into
 * fixed size static memory like shared-memory and memory-mapped file.
 * The creator qhasharr() initializes static memory to makes small slots in it.
 * The default slot size factors are defined in Q_HASHARR_KEYSIZE and
 * Q_HASHARR_VALUESIZE. And they are applied at compile time.
 *
 * The value part of an element will be stored across several slots if it's size
 * exceeds the slot size. But the key part of an element will be truncated if
 * the size exceeds and it's length and more complex MD5 hash value will be
 * stored with the key. So to look up a particular key, first we find an element
 * which has same hash value. If the key was not truncated, we just do key
 * comparison. But if the key was truncated because it's length exceeds, we do
 * both md5 and key comparison(only stored size) to verify that the key is same.
 * So please be aware of that, theoretically there is a possibility we pick
 * wrong element in case a key exceeds the limit, has same length and MD5 hash
 * with lookup key. But this possibility is very low and almost zero in practice.
 *
 * qhasharr hash-table does not provide thread-safe handling intentionally and
 * let users determine whether to provide locking mechanism or not, depending on
 * the use cases. When there's race conditions expected, you should provide a
 * shared resource control using mutex or semaphore to make sure data gets
 * updated by one instance at a time.
 *
 * @code
 *  [Data Structure Diagram]
 *
 *  +--[Static Flat Memory Area]-----------------------------------------------+
 *  | +-[Header]---------+ +-[Slot 0]---+ +-[Slot 1]---+        +-[Slot N]---+ |
 *  | |Private table data| |KEY A|DATA A| |KEY B|DATA B|  ....  |KEY N|DATA N| |
 *  | +------------------+ +------------+ +------------+        +------------+ |
 *  +--------------------------------------------------------------------------+
 *
 *  Below diagram shows how a big value is stored.
 *  +--[Static Flat Memory Area------------------------------------------------+
 *  | +--------+ +-[Slot 0]---+ +-[Slot 1]---+ +-[Slot 2]---+ +-[Slot 3]-----+ |
 *  | |TBL INFO| |KEY A|DATA A| |DATA A cont.| |KEY B|DATA B| |DATA A cont.  | |
 *  | +--------+ +------------+ +------------+ +------------+ +--------------+ |
 *  |                      ^~~link~~^     ^~~~~~~~~~link~~~~~~~~~^             |
 *  +--------------------------------------------------------------------------+
 * @endcode
 *
 * @code
 *  // initialize hash-table.
 *  char memory[1000 * 10];
 *  qhasharr_t *tbl = qhasharr(memory, sizeof(memory));
 *  if(tbl == NULL) return;
 *
 *  // insert elements (key duplication does not allowed)
 *  tbl->putstr(tbl, "e1", "a");
 *  tbl->putstr(tbl, "e2", "b");
 *  tbl->putstr(tbl, "e3", "c");
 *
 *  // debug print out
 *  tbl->debug(tbl, stdout);
 *
 *  char *e2 = tbl->getstr(tbl, "e2");
 *  if(e2 != NULL) {
 *     printf("getstr('e2') : %s\n", e2);
 *     free(e2);
 *  }
 *
 *  // Release reference object.
 *  tbl->free(tbl);
 * @endcode
 *
 * An example for using hash table over shared memory.
 *
 * @code
 *  [CREATOR SIDE]
 *  int maxslots = 1000;
 *  int memsize = qhasharr_calculate_memsize(maxslots);
 *
 *  // create shared memory
 *  int shmid = qshm_init("/tmp/some_id_file", 'q', memsize, true);
 *  if(shmid < 0) return -1; // creation failed
 *  void *memory = qshm_get(shmid);
 *
 *  // initialize hash-table
 *  qhasharr_t *tbl = qhasharr(memory, memsize);
 *  if(hasharr == NULL) return -1;
 *
 *  (...your codes with your own locking mechanism...)
 *
 *  // Release reference object
 *  tbl->free(tbl);
 *
 *  // destroy shared memory
 *  qshm_free(shmid);
 *
 *  [USER SIDE]
 *  int shmid = qshm_getid("/tmp/some_id_file", 'q');
 *
 *  // get shared memory
 *  void *memory = qshm_get(shmid);
 *
 *  // map existing memory into table
 *  qhasharr_t *tbl = qhasharr(memory, 0);
 *
 *  (...your codes with your own locking mechanism...)
 *
 *  // Release reference object
 *  tbl->free(tbl);
 * @endcode
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include "qinternal.h"
#include "utilities/qhash.h"
#include "containers/qhasharr.h"

#ifndef _DOXYGEN_SKIP

// internal usages
static qhasharr_slot_t* get_slots(qhasharr_t *tbl);
static int find_avail(qhasharr_t *tbl, int startidx);
static int get_idx(qhasharr_t *tbl, const char *key, unsigned int hash);
static void *get_data(qhasharr_t *tbl, int idx, size_t *size);
static bool put_data(qhasharr_t *tbl, int idx, unsigned int hash,
                      const char *key, const void *value, size_t size,
                      int count);
static bool copy_slot(qhasharr_t *tbl, int idx1, int idx2);
static bool remove_slot(qhasharr_t *tbl, int idx);
static bool remove_data(qhasharr_t *tbl, int idx);

#endif

/**
 * Get how much memory is needed for N slots.
 *
 * @param max       a number of maximum internal slots
 *
 * @return memory size needed
 *
 * @note
 *  This can be used for calculating minimum memory size for N slots.
 */
size_t qhasharr_calculate_memsize(int max) {
    size_t memsize = sizeof(qhasharr_data_t)
            + (sizeof(qhasharr_slot_t) * (max));
    return memsize;
}

/**
 * Initialize static hash table
 *
 * @param memory    a pointer of data memory.
 * @param memsize   a size of data memory, 0 for using existing data.
 *
 * @return qhasharr_t container pointer, otherwise returns NULL.
 * @retval errno  will be set in error condition.
 *  - EINVAL : Assigned memory is too small. It must bigger enough to allocate
 *  at least 1 slot.
 *
 * @code
 *  // initialize hash-table with 100 slots.
 *  // A single element can take several slots.
 *  char memory[112 * 100];
 *
 *  // Initialize new table.
 *  qhasharr_t *tbl = qhasharr(memory, sizeof(memory));
 *
 *  // Use existing table.
 *  qhasharr_t *tbl2 = qhasharr(memory, 0);
 * @endcode
 */
qhasharr_t *qhasharr(void *memory, size_t memsize) {
    // Structure memory.
    qhasharr_data_t *data = (qhasharr_data_t *) memory;

    // Initialize data if memsize is set or use existing data.
    if (memsize > 0) {
        // calculate max
        int maxslots = (memsize - sizeof(qhasharr_data_t))
                / sizeof(qhasharr_slot_t);
        if (maxslots < 1 || memsize <= sizeof(qhasharr_t)) {
            errno = EINVAL;
            return NULL;
        }

        // Set memory.
        memset((void *) data, 0, memsize);
        data->maxslots = maxslots;
        data->usedslots = 0;
        data->num = 0;
    }

    // Create the table object.
    qhasharr_t *tbl = (qhasharr_t *) malloc(sizeof(qhasharr_t));
    if (tbl == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    memset((void *) tbl, 0, sizeof(qhasharr_t));

    // assign methods
    tbl->put = qhasharr_put;
    tbl->putstr = qhasharr_putstr;
    tbl->putstrf = qhasharr_putstrf;
    tbl->putint = qhasharr_putint;

    tbl->get = qhasharr_get;
    tbl->getstr = qhasharr_getstr;
    tbl->getint = qhasharr_getint;

    tbl->remove = qhasharr_remove;
    tbl->remove_by_idx = qhasharr_remove_by_idx;

    tbl->getnext = qhasharr_getnext;

    tbl->size = qhasharr_size;
    tbl->clear = qhasharr_clear;
    tbl->debug = qhasharr_debug;

    tbl->free = qhasharr_free;

    tbl->data = data;

    return tbl;
}

/**
 * qhasharr->put(): Put an object into this table.
 *
 * @param tbl       qhasharr_t container pointer.
 * @param key       key string
 * @param value     value object data
 * @param size      size of value
 *
 * @return true if successful, otherwise returns false
 * @retval errno will be set in error condition.
 *  - ENOBUFS   : Table doesn't have enough space to store the object.
 *  - EINVAL    : Invalid argument.
 *  - EFAULT    : Unexpected error. Data structure is not constant.
 */
bool qhasharr_put(qhasharr_t *tbl, const char *key, const void *value,
                size_t size) {
    if (tbl == NULL || key == NULL || value == NULL) {
        errno = EINVAL;
        return false;
    }

    qhasharr_data_t *data = tbl->data;
    qhasharr_slot_t *tblslots = get_slots(tbl);

    // check full
    if (data->usedslots >= data->maxslots) {
        DEBUG("hasharr: put %s - FULL", key);
        errno = ENOBUFS;
        return false;
    }

    // get hash integer
    unsigned int hash = qhashmurmur3_32(key, strlen(key)) % data->maxslots;

    // check, is slot empty
    if (tblslots[hash].count == 0) {  // empty slot
        // put data
        if (put_data(tbl, hash, hash, key, value, size, 1) == false) {
            DEBUG("hasharr: FAILED put(new) %s", key);
            return false;
        }DEBUG("hasharr: put(new) %s (idx=%d,hash=%u,tot=%d)", key, hash, hash,
               data->usedslots);
    } else if (tblslots[hash].count > 0) {  // same key or hash collision
        // check same key;
        int idx = get_idx(tbl, key, hash);
        if (idx >= 0) {  // same key
            // remove and recall
            qhasharr_remove(tbl, key);
            return qhasharr_put(tbl, key, value, size);
        } else {  // no same key, just hash collision
            // find empty slot
            int idx = find_avail(tbl, hash);
            if (idx < 0) {
                errno = ENOBUFS;
                return false;
            }

            // put data. -1 is used for collision resolution (idx != hash);
            if (put_data(tbl, idx, hash, key, value, size, -1) == false) {
                DEBUG("hasharr: FAILED put(col) %s", key);
                return false;
            }

            // increase counter from leading slot
            tblslots[hash].count++;

            DEBUG("hasharr: put(col) %s (idx=%d,hash=%u,tot=%d)", key, idx,
                  hash, data->usedslots);
        }
    } else {
        // in case of -1 or -2, move it. -1 used for collision resolution,
        // -2 used for extended value data.

        // find empty slot
        int idx = find_avail(tbl, hash + 1);
        if (idx < 0) {
            errno = ENOBUFS;
            return false;
        }

        // move dup slot to empty
        copy_slot(tbl, idx, hash);
        remove_slot(tbl, hash);

        // if moved slot is extended data slot, adjust the link chain from the mother.
        if (tblslots[idx].count == -2) {
            tblslots[tblslots[idx].hash].link = idx;
            if (tblslots[idx].link != -1) {
                tblslots[tblslots[idx].link].hash = idx;
            }
        }

        // store data
        if (put_data(tbl, hash, hash, key, value, size, 1) == false) {
            DEBUG("hasharr: FAILED put(swp) %s", key);
            return false;
        }

        DEBUG("hasharr: put(swp) %s (idx=%u,hash=%u,tot=%d)", key, hash, hash,
              data->usedslots);
    }

    return true;
}

/**
 * qhasharr->putstr(): Put a string into this table
 *
 * @param tbl       qhasharr_t container pointer.
 * @param key       key string.
 * @param value     string data.
 *
 * @return true if successful, otherwise returns false
 * @retval errno will be set in error condition.
 *  - ENOBUFS   : Table doesn't have enough space to store the object.
 *  - EINVAL    : Invalid argument.
 *  - EFAULT    : Unexpected error. Data structure is not constant.
 */
bool qhasharr_putstr(qhasharr_t *tbl, const char *key, const char *str) {
    int size = (str != NULL) ? (strlen(str) + 1) : 0;
    return qhasharr_put(tbl, key, (void *) str, size);
}

/**
 * qhasharr->putstrf(): Put a formatted string into this table.
 *
 * @param tbl       qhasharr_t container pointer.
 * @param key       key name.
 * @param format    formatted string data.
 *
 * @return true if successful, otherwise returns false.
 * @retval errno will be set in error condition.
 *  - ENOBUFS   : Table doesn't have enough space to store the object.
 *  - ENOMEM    : System memory allocation failure.
 *  - EINVAL    : Invalid argument.
 *  - EFAULT    : Unexpected error. Data structure is not constant.
 */
bool qhasharr_putstrf(qhasharr_t *tbl, const char *key, const char *format, ...) {
    char *str;
    DYNAMIC_VSPRINTF(str, format);
    if (str == NULL) {
        errno = ENOMEM;
        return false;
    }

    bool ret = qhasharr_putstr(tbl, key, str);
    free(str);
    return ret;
}

/**
 * qhasharr->putint(): Put an integer into this table as string type.
 *
 * @param tbl       qhasharr_t container pointer.
 * @param key       key string
 * @param value     value integer
 *
 * @return true if successful, otherwise returns false
 * @retval errno will be set in error condition.
 *  - ENOBUFS   : Table doesn't have enough space to store the object.
 *  - EINVAL    : Invalid argument.
 *  - EFAULT    : Unexpected error. Data structure is not constant.
 *
 * @note
 * The integer will be converted to a string object and stored as string object.
 */
bool qhasharr_putint(qhasharr_t *tbl, const char *key, int64_t num) {
    char str[20 + 1];
    snprintf(str, sizeof(str), "%"PRId64, num);
    return qhasharr_putstr(tbl, key, str);
}

/**
 * qhasharr->get(): Get an object from this table
 *
 * @param tbl       qhasharr_t container pointer.
 * @param key       key string
 * @param size      if not NULL, oject size will be stored
 *
 * @return malloced object pointer if successful, otherwise(not found)
 *  returns NULL
 * @retval errno will be set in error condition.
 *  - ENOENT    : No such key found.
 *  - EINVAL    : Invalid argument.
 *  - ENOMEM    : Memory allocation failed.
 *
 * @note
 * returned object must be freed after done using.
 */
void *qhasharr_get(qhasharr_t *tbl, const char *key, size_t *size) {
    if (tbl == NULL || key == NULL) {
        errno = EINVAL;
        return NULL;
    }

    qhasharr_data_t *data = tbl->data;

    // get hash integer
    unsigned int hash = qhashmurmur3_32(key, strlen(key)) % data->maxslots;

    int idx = get_idx(tbl, key, hash);
    if (idx < 0) {
        errno = ENOENT;
        return NULL;
    }

    return get_data(tbl, idx, size);
}

/**
 * qhasharr->getstr(): Finds an object with given name and returns as
 * string type.
 *
 * @param tbl       qhasharr_t container pointer.
 * @param key       key string
 *
 * @return string pointer if successful, otherwise(not found) returns NULL
 * @retval errno will be set in error condition.
 *  - ENOENT    : No such key found.
 *  - EINVAL    : Invalid argument.
 *  - ENOMEM    : Memory allocation failed.
 *
 * @note
 * returned object must be freed after done using.
 */
char *qhasharr_getstr(qhasharr_t *tbl, const char *key) {
    return (char *)qhasharr_get(tbl, key, NULL);
}

/**
 * qhasharr->getint(): Finds an object with given name and returns as
 * integer type.
 *
 * @param tbl       qhasharr_t container pointer.
 * @param key       key string
 *
 * @return value integer if successful, otherwise(not found) returns 0
 * @retval errno will be set in error condition.
 *  - ENOENT    : No such key found.
 *  - EINVAL    : Invalid argument.
 *  - ENOMEM    : Memory allocation failed.
 */
int64_t qhasharr_getint(qhasharr_t *tbl, const char *key) {
    int64_t num = 0;
    char *str = qhasharr_getstr(tbl, key);
    if (str != NULL) {
        num = atoll(str);
        free(str);
    }

    return num;
}

/**
 * qhasharr->remove(): Remove an object from this table.
 *
 * @param tbl       qhasharr_t container pointer.
 * @param key       key string
 *
 * @return true if successful, otherwise(not found) returns false
 * @retval errno will be set in error condition.
 *  - ENOENT    : No such key found.
 *  - EINVAL    : Invald argument.
 *  - EFAULT    : Unexpected error. Data structure is not constant.
 */
bool qhasharr_remove(qhasharr_t *tbl, const char *key) {
    if (tbl == NULL || key == NULL) {
        errno = EINVAL;
        return false;
    }

    qhasharr_data_t *data = tbl->data;

    // get hash integer
    unsigned int hash = qhashmurmur3_32(key, strlen(key)) % data->maxslots;
    int idx = get_idx(tbl, key, hash);
    if (idx < 0) {
        DEBUG("key(%s) not found.", key);
        errno = ENOENT;
        return false;
    }

    return qhasharr_remove_by_idx(tbl, idx);
}

/**
 * qhasharr->getnext(): Get next element.
 *
 * @param tbl       qhasharr_t container pointer.
 * @param idx       index pointer
 *
 * @return key name string if successful, otherwise(end of table) returns NULL
 * @retval errno will be set in error condition.
 *  - ENOENT    : No next element.
 *  - EINVAL    : Invald argument.
 *  - ENOMEM    : Memory allocation failed.
 *
 * @code
 *  int idx = 0;
 *  qhasharr_obj_t obj;
 *  while(tbl->getnext(tbl, &obj, &idx) == true) {
 *    printf("NAME=%s, DATA=%s, SIZE=%zu\n",
 *           obj.name, (char*)obj.data, obj.size);
 *    free(obj.name);
 *    free(obj.data);
 *  }
 * @endcode
 *
 * @note
 *  Please be aware a key name will be returned with truncated length
 *  because key name gets truncated if it doesn't fit into slot size,
 *  Q_HASHARR_KEYSIZE.
 */
bool qhasharr_getnext(qhasharr_t *tbl, qhasharr_obj_t *obj, int *idx) {
    if (tbl == NULL || obj == NULL || idx == NULL) {
        errno = EINVAL;
        return NULL;
    }

    qhasharr_data_t *data = tbl->data;
    qhasharr_slot_t *tblslots = get_slots(tbl);

    for (; *idx < data->maxslots; (*idx)++) {
        if (tblslots[*idx].count == 0 || tblslots[*idx].count == -2) {
            continue;
        }

        size_t keylen = tblslots[*idx].data.pair.keylen;
        if (keylen > Q_HASHARR_KEYSIZE)
            keylen = Q_HASHARR_KEYSIZE;

        obj->name = (char *) malloc(keylen + 1);
        if (obj->name == NULL) {
            errno = ENOMEM;
            return false;
        }
        memcpy(obj->name, tblslots[*idx].data.pair.key, keylen);
        obj->name[keylen] = '\0';

        obj->data = get_data(tbl, *idx, &obj->size);
        if (obj->data == NULL) {
            free(obj->name);
            errno = ENOMEM;
            return false;
        }

        *idx += 1;
        return true;
    }

    errno = ENOENT;
    return false;
}

/**
 * qhasharr->remove_by_idx(): Remove an object from this table by index number.
 *
 * @param tbl       qhasharr_t container pointer.
 * @param key       key string
 *
 * @return true if successful, otherwise(not found) returns false
 * @retval errno will be set in error condition.
 *  - ENOENT    : Index is not pointing a valid object.
 *  - EINVAL    : Invald argument.
 *  - EFAULT    : Unexpected error. Data structure is not constant.
 *
 * @code
 *  int idx = 0;
 *  qhasharr_obj_t obj;
 *  while(tbl->getnext(tbl, &obj, &idx) == true) {
 *    if (condition_to_remove) {
 *      idx--;  // adjust index by -1
 *      remove_by_idx(idx);
 *    }
 *    free(obj.name);
 *    free(obj.data);
 *  }
 * @endcode
 *
 * @note
 * This function is to remove an object in getnext() traversal loop without
 * knowing the object name but index value. When key names are longer than
 * defined size, the keys are stored with truncation with their fingerprint,
 * so this method provides a way to remove those keys.
 * getnext() returns actual index + 1(pointing next slot of current finding),
 * so you need to adjust it by -1 for the valid index number. And once you
 * remove object by this method, rewind idx by -1 before calling getnext()
 * because collision objects can be moved back to removed index again, so
 * by adjusting index by -1, getnext() can continue search from the removed
 * slot index again. Please refer an example code.
 */
bool qhasharr_remove_by_idx(qhasharr_t *tbl, int idx) {
    if (idx < 0) {
        DEBUG("Invalid index range, %d", idx);
        errno = EINVAL;
        ;
        return false;
    }

    qhasharr_data_t *data = tbl->data;
    qhasharr_slot_t *tblslots = get_slots(tbl);

    if (tblslots[idx].count == 1) {
        // just remove
        remove_data(tbl, idx);
        DEBUG("hasharr: remove_by_idx (idx=%d,tot=%d)", idx, data->usedslots);
    } else if (tblslots[idx].count > 1) {  // leading slot and has collision
        // find the collision key
        int idx2;
        for (idx2 = idx + 1;; idx2++) {
            if (idx2 >= data->maxslots)
                idx2 = 0;
            if (idx2 == idx) {
                DEBUG("hasharr: [BUG] failed to remove dup key at %d.", idx);
                errno = EFAULT;
                return false;
            }
            if (tblslots[idx2].count == -1
                    && tblslots[idx2].hash == tblslots[idx].hash) {
                break;
            }
        }

        // move to leading slot
        int backupcount = tblslots[idx].count;
        remove_data(tbl, idx);  // remove leading data
        copy_slot(tbl, idx, idx2);  // copy slot
        remove_slot(tbl, idx2);  // remove moved slot

        tblslots[idx].count = backupcount - 1;  // adjust collision counter
        if (tblslots[idx].link != -1) {
            tblslots[tblslots[idx].link].hash = idx;
        }

        DEBUG("hasharr: remove_by_idx(lead) (idx=%d,tot=%d)", idx,
              data->usedslots);
    } else if (tblslots[idx].count == -1) {  // collision key
        // decrease counter from leading slot
        if (tblslots[tblslots[idx].hash].count <= 1) {
            DEBUG("hasharr: [BUG] failed to remove at %d. counter of leading slot mismatch.",
                  idx);
            errno = EFAULT;
            return false;
        }
        tblslots[tblslots[idx].hash].count--;

        // remove data
        remove_data(tbl, idx);
        DEBUG("hasharr: remove_by_idx(dup) (idx=%d,tot=%d)", idx,
              data->usedslots);
    } else {
        DEBUG("Index(%d) is not pointing a valid object", idx);
        errno = ENOENT;
        ;
        return false;
    }

    return true;
}

/**
 * qhasharr->size(): Returns the number of objects in this table.
 *
 * @param tbl       qhasharr_t container pointer.
 *
 * @return a number of elements stored.
 */
int qhasharr_size(qhasharr_t *tbl, int *maxslots, int *usedslots) {
    if (tbl == NULL) {
        errno = EINVAL;
        return -1;
    }

    qhasharr_data_t *data = tbl->data;

    if (maxslots != NULL)
        *maxslots = data->maxslots;
    if (usedslots != NULL)
        *usedslots = data->usedslots;

    return data->num;
}

/**
 * qhasharr->clear(): Clears this table so that it contains no keys.
 *
 * @param tbl       qhasharr_t container pointer.
 *
 * @return true if successful, otherwise returns false.
 */
void qhasharr_clear(qhasharr_t *tbl) {
    if (tbl == NULL) {
        errno = EINVAL;
        return;
    }

    qhasharr_data_t *data = tbl->data;
    qhasharr_slot_t *tblslots = get_slots(tbl);

    if (data->usedslots == 0)
        return;

    data->usedslots = 0;
    data->num = 0;

    // clear memory
    memset((void *) tblslots, '\0', (data->maxslots * sizeof(qhasharr_slot_t)));
}

/**
 * qhasharr->debug(): Print hash table for debugging purpose
 *
 * @param tbl       qhasharr_t container pointer.
 * @param out       output stream
 *
 * @return true if successful, otherwise returns false
 * @retval errno will be set in error condition.
 *  - EIO       : Invalid output stream.
 */
bool qhasharr_debug(qhasharr_t *tbl, FILE *out) {
    if (tbl == NULL) {
        errno = EINVAL;
        return false;
    }

    if (out == NULL) {
        errno = EIO;
        return false;
    }

    qhasharr_slot_t *tblslots = get_slots(tbl);

    int idx = 0;
    qhasharr_obj_t obj;
    while (tbl->getnext(tbl, &obj, &idx) == true) {
        uint16_t keylen = tblslots[idx - 1].data.pair.keylen;
        fprintf(out, "%s%s(%d)=", obj.name,
                (keylen > Q_HASHARR_KEYSIZE) ? "..." : "", keylen);
        _q_textout(out, obj.data, obj.size, MAX_HUMANOUT);
        fprintf(out, " (%zu)\n", obj.size);

        free(obj.name);
        free(obj.data);
    }

#ifdef BUILD_DEBUG
    qhasharr_data_t *data = tbl->data;
    fprintf(out, "%d elements (slot %d used/%d total)\n",
            data->num, data->usedslots, data->maxslots);
    for (idx = 0; idx < data->maxslots; idx++) {
        if (tblslots[idx].count == 0) continue;

        fprintf(out, "slot=%d,type=", idx);
        if (tblslots[idx].count == -2) {
            fprintf(out, "EXTEND,prev=%d,next=%d,data=",
                    tblslots[idx].hash, tblslots[idx].link);
            _q_textout(out,
                    tblslots[idx].data.ext.value,
                    tblslots[idx].size,
                    MAX_HUMANOUT);
            fprintf(out, ",size=%d", tblslots[idx].size);
        } else {
            fprintf(out, "%s", (tblslots[idx].count == -1)?"COLISN":"NORMAL");
            fprintf(out, ",count=%d,hash=%u,key=",
                    tblslots[idx].count, tblslots[idx].hash);
            _q_textout(out,
                    tblslots[idx].data.pair.key,
                    (tblslots[idx].data.pair.keylen>Q_HASHARR_KEYSIZE)
                    ? Q_HASHARR_KEYSIZE
                    : tblslots[idx].data.pair.keylen,
                    MAX_HUMANOUT);
            fprintf(out, ",keylen=%d,data=", tblslots[idx].data.pair.keylen);
            _q_textout(out,
                    tblslots[idx].data.pair.value,
                    tblslots[idx].size,
                    MAX_HUMANOUT);
            fprintf(out, ",size=%d", tblslots[idx].size);
        }
        fprintf(out, "\n");
    }
#endif

    return true;
}

/**
 * qhasharr->free(): De-allocate table reference object.
 *
 * @param tbl   qhashtbl_t container pointer.
 *
 * @note
 *  This does not de-allocate memory but only function reference object.
 *  Data memory such as shared memory must be de-allocated separately.
 */
void qhasharr_free(qhasharr_t *tbl) {
    free(tbl);
}

#ifndef _DOXYGEN_SKIP

static qhasharr_slot_t* get_slots(qhasharr_t *tbl) {
    return (qhasharr_slot_t*) ((char*) (tbl->data) + sizeof(qhasharr_data_t));
}

// find empty slot : return empty slow number, otherwise returns -1.
static int find_avail(qhasharr_t *tbl, int startidx) {
    qhasharr_data_t *data = tbl->data;
    qhasharr_slot_t *tblslots = get_slots(tbl);

    if (startidx >= data->maxslots)
        startidx = 0;

    int idx = startidx;
    while (true) {
        if (tblslots[idx].count == 0)
            return idx;

        idx++;
        if (idx >= data->maxslots)
            idx = 0;
        if (idx == startidx)
            break;
    }

    return -1;
}

static int get_idx(qhasharr_t *tbl, const char *key, unsigned int hash) {
    qhasharr_data_t *data = tbl->data;
    qhasharr_slot_t *tblslots = get_slots(tbl);

    if (tblslots[hash].count > 0) {
        int count, idx;
        for (count = 0, idx = hash; count < tblslots[hash].count;) {
            if (tblslots[idx].hash == hash
                    && (tblslots[idx].count > 0 || tblslots[idx].count == -1)) {
                // same hash
                count++;

                // is same key?
                size_t keylen = strlen(key);
                // first check key length
                if (keylen == tblslots[idx].data.pair.keylen) {
                    if (keylen <= Q_HASHARR_KEYSIZE) {
                        // original key is stored
                        if (!memcmp(key, tblslots[idx].data.pair.key, keylen)) {
                            return idx;
                        }
                    } else {
                        // key is truncated, compare MD5 also.
                        unsigned char keymd5[16];
                        qhashmd5(key, keylen, keymd5);
                        if (!memcmp(key, tblslots[idx].data.pair.key,
                        Q_HASHARR_KEYSIZE)
                                && !memcmp(keymd5,
                                           tblslots[idx].data.pair.keymd5,
                                           16)) {
                            return idx;
                        }
                    }
                }
            }

            // increase idx
            idx++;
            if (idx >= data->maxslots)
                idx = 0;

            // check loop
            if (idx == hash)
                break;

            continue;
        }
    }

    return -1;
}

static void *get_data(qhasharr_t *tbl, int idx, size_t *size) {
    if (idx < 0) {
        errno = ENOENT;
        return NULL;
    }

    //qhasharr_data_t *data = tbl->data;
    qhasharr_slot_t *tblslots = get_slots(tbl);

    int newidx;
    size_t valsize;
    for (newidx = idx, valsize = 0;; newidx = tblslots[newidx].link) {
        valsize += tblslots[newidx].size;
        if (tblslots[newidx].link == -1)
            break;
    }

    void *value, *vp;
    value = malloc(valsize);
    if (value == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    for (newidx = idx, vp = value;; newidx = tblslots[newidx].link) {
        if (tblslots[newidx].count == -2) {
            // extended data block
            memcpy(vp, (void *) tblslots[newidx].data.ext.value,
                   tblslots[newidx].size);
        } else {
            // key/value pair data block
            memcpy(vp, (void *) tblslots[newidx].data.pair.value,
                   tblslots[newidx].size);
        }

        vp += tblslots[newidx].size;
        if (tblslots[newidx].link == -1)
            break;
    }

    if (size != NULL)
        *size = valsize;
    return value;
}

static bool put_data(qhasharr_t *tbl, int idx, unsigned int hash,
                      const char *key, const void *value, size_t size,
                      int count) {
    qhasharr_data_t *data = tbl->data;
    qhasharr_slot_t *tblslots = get_slots(tbl);

    // check if used
    if (tblslots[idx].count != 0) {
        DEBUG("hasharr: BUG found.");
        errno = EFAULT;
        return false;
    }

    size_t keylen = strlen(key);
    unsigned char keymd5[16];
    qhashmd5(key, keylen, keymd5);

    // store key
    tblslots[idx].count = count;
    tblslots[idx].hash = hash;
    strncpy(tblslots[idx].data.pair.key, key, Q_HASHARR_KEYSIZE);
    memcpy((char *) tblslots[idx].data.pair.keymd5, (char *) keymd5, 16);
    tblslots[idx].data.pair.keylen = keylen;
    tblslots[idx].link = -1;

    // store value
    int newidx;
    size_t savesize;
    for (newidx = idx, savesize = 0; savesize < size;) {
        if (savesize > 0) {  // find next empty slot
            int tmpidx = find_avail(tbl, newidx + 1);
            if (tmpidx < 0) {
                DEBUG("hasharr: Can't expand slot for key %s.", key);
                remove_data(tbl, idx);
                errno = ENOBUFS;
                return false;
            }

            // clear & set
            memset((void *) (&tblslots[tmpidx]), '\0', sizeof(qhasharr_slot_t));

            tblslots[tmpidx].count = -2;      // extended data block
            tblslots[tmpidx].hash = newidx;   // prev link
            tblslots[tmpidx].link = -1;       // end block mark
            tblslots[tmpidx].size = 0;

            tblslots[newidx].link = tmpidx;   // link chain

            DEBUG("hasharr: slot %d is linked to slot %d for key %s.", tmpidx,
                  newidx, key);
            newidx = tmpidx;
        }

        // copy data
        size_t copysize = size - savesize;

        if (tblslots[newidx].count == -2) {
            // extended value
            if (copysize > sizeof(struct Q_HASHARR_SLOT_EXT)) {
                copysize = sizeof(struct Q_HASHARR_SLOT_EXT);
            }
            memcpy(tblslots[newidx].data.ext.value, value + savesize, copysize);
        } else {
            // first slot
            if (copysize > Q_HASHARR_VALUESIZE) {
                copysize = Q_HASHARR_VALUESIZE;
            }
            memcpy(tblslots[newidx].data.pair.value, value + savesize,
                   copysize);

            // increase stored key counter
            data->num++;
        }
        tblslots[newidx].size = copysize;
        savesize += copysize;

        // increase used slot counter
        data->usedslots++;
    }

    return true;
}

static bool copy_slot(qhasharr_t *tbl, int idx1, int idx2) {
    qhasharr_data_t *data = tbl->data;
    qhasharr_slot_t *tblslots = get_slots(tbl);

    if (tblslots[idx1].count != 0 || tblslots[idx2].count == 0) {
        DEBUG("hasharr: BUG found.");
        errno = EFAULT;
        return false;
    }

    memcpy((void *) (&tblslots[idx1]), (void *) (&tblslots[idx2]),
           sizeof(qhasharr_slot_t));

    // increase used slot counter
    data->usedslots++;

    return true;
}

static bool remove_slot(qhasharr_t *tbl, int idx) {
    qhasharr_data_t *data = tbl->data;
    qhasharr_slot_t *tblslots = get_slots(tbl);

    if (tblslots[idx].count == 0) {
        DEBUG("hasharr: BUG found.");
        errno = EFAULT;
        return false;
    }

    tblslots[idx].count = 0;

    // decrease used slot counter
    data->usedslots--;

    return true;
}

static bool remove_data(qhasharr_t *tbl, int idx) {
    qhasharr_data_t *data = tbl->data;
    qhasharr_slot_t *tblslots = get_slots(tbl);

    if (tblslots[idx].count == 0) {
        DEBUG("hasharr: BUG found.");
        errno = EFAULT;
        return false;
    }

    while (true) {
        int link = tblslots[idx].link;
        remove_slot(tbl, idx);

        if (link == -1)
            break;

        idx = link;
    }

    // decrease stored key counter
    data->num--;

    return true;
}

#endif /* _DOXYGEN_SKIP */
