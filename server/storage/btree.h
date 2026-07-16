/*
 * ============================================================================
 * Event Ticketing Platform — B+ Tree Index (Header)
 * ============================================================================
 * In-memory B+ Tree with disk persistence. Provides O(log n) key lookups
 * mapping uint32_t primary keys to record_ptr_t (page_id + slot_id).
 *
 * Design:
 *   - Internal nodes store keys + child pointers
 *   - Leaf nodes store keys + values + next-leaf pointer (for range scans)
 *   - Tree is kept in memory for fast operations
 *   - Serialized to a binary .idx file on close / save
 *   - Deserialized from .idx file on open
 * ============================================================================
 */

#ifndef ETP_BTREE_H
#define ETP_BTREE_H

#include "common/types.h"
#include <stdint.h>

/* ================================================================
 * B+ Tree Node
 * ================================================================ */
typedef struct btree_node {
    int                     is_leaf;    /* 1 = leaf, 0 = internal           */
    int                     num_keys;   /* Current number of keys           */
    uint32_t               *keys;       /* Key array [capacity: order - 1]  */

    /* Leaf nodes: values + next pointer. Internal nodes: child pointers.   */
    union {
        struct btree_node **children;   /* Internal: child array [order]    */
        record_ptr_t       *values;     /* Leaf: value array [order - 1]    */
    };
    struct btree_node      *next;       /* Leaf: next leaf (NULL for internal) */
} btree_node_t;

/* ================================================================
 * B+ Tree Handle
 * ================================================================ */
typedef struct btree {
    btree_node_t   *root;              /* Root node                         */
    int             order;             /* Maximum children per internal node */
    uint32_t        size;              /* Total number of key-value pairs    */
    char            filename[256];     /* Path to .idx persistence file      */
    int             dirty;             /* 1 if unsaved changes exist         */
} btree_t;

/* B+ Tree file magic number */
#define BTREE_MAGIC  0x42505400  /* "BPT\0" */

/* ================================================================
 * Lifecycle
 * ================================================================ */

/* Create a new empty B+ Tree. Writes an empty index file. */
btree_t *btree_create(const char *filename, int order);

/* Open an existing B+ Tree from a .idx file. */
btree_t *btree_open(const char *filename);

/* Save to disk (if dirty) and free all memory. */
void     btree_close(btree_t *tree);

/* ================================================================
 * Core Operations
 * ================================================================ */

/* Insert a key-value pair. Returns 0 on success, -1 if key already exists. */
int      btree_insert(btree_t *tree, uint32_t key, record_ptr_t value);

/* Search for a key. Returns 0 if found (result written to *out), -1 if not. */
int      btree_search(btree_t *tree, uint32_t key, record_ptr_t *out);

/* Delete a key. Returns 0 on success, -1 if not found.
 * Uses lazy deletion (no rebalancing — acceptable for this project scope). */
int      btree_delete(btree_t *tree, uint32_t key);

/* Update the value for an existing key. Returns 0 on success, -1 if not found. */
int      btree_update(btree_t *tree, uint32_t key, record_ptr_t new_value);

/* ================================================================
 * Range Query
 * ================================================================ */

/* Find all keys in [low, high] inclusive. Writes up to max_results into results[].
 * Returns the number of results found. */
int      btree_range_scan(btree_t *tree, uint32_t low, uint32_t high,
                          record_ptr_t *results, int max_results);

/* ================================================================
 * Persistence
 * ================================================================ */

/* Explicitly save tree to disk. Called automatically by btree_close(). */
int      btree_save(btree_t *tree);

/* ================================================================
 * Utility
 * ================================================================ */

uint32_t btree_size(btree_t *tree);

/* Print tree structure to stderr (for debugging / viva demo). */
void     btree_print(btree_t *tree);

#endif /* ETP_BTREE_H */
