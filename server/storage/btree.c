/*
 * ============================================================================
 * Event Ticketing Platform — B+ Tree Index (Implementation)
 * ============================================================================
 * Full B+ Tree implementation with insert (with splits), search, lazy delete,
 * range scan, and binary file persistence.
 *
 * Key design decisions:
 *   - Lazy deletion: keys are removed from leaves without rebalancing.
 *     Production B+ Trees would merge underflowing nodes, but lazy delete
 *     is acceptable for this project's data volume.
 *   - In-memory: entire tree lives in RAM for fast operations. Persisted
 *     to disk via pre-order serialization on save/close.
 *   - Leaf chaining: leaves are linked via `next` pointers for efficient
 *     range scans — a core B+ Tree advantage over B-Trees.
 * ============================================================================
 */

#include "btree.h"
#include "common/config.h"
#include "common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Internal Helpers — Node Allocation
 * ================================================================ */

static btree_node_t *node_create(int order, int is_leaf) {
    btree_node_t *node = calloc(1, sizeof(btree_node_t));
    if (!node) {
        etp_log(LOG_ERROR, "btree: failed to allocate node");
        return NULL;
    }

    node->is_leaf  = is_leaf;
    node->num_keys = 0;
    node->next     = NULL;

    /* Allocate key array — max (order - 1) keys */
    node->keys = calloc(order, sizeof(uint32_t));  /* one extra for overflow during split */
    if (!node->keys) {
        free(node);
        return NULL;
    }

    if (is_leaf) {
        /* Leaf: allocate value array */
        node->values = calloc(order, sizeof(record_ptr_t));  /* one extra for overflow */
        if (!node->values) {
            free(node->keys);
            free(node);
            return NULL;
        }
    } else {
        /* Internal: allocate child pointer array */
        node->children = calloc(order + 1, sizeof(btree_node_t *));  /* one extra for overflow */
        if (!node->children) {
            free(node->keys);
            free(node);
            return NULL;
        }
    }

    return node;
}

static void node_free(btree_node_t *node) {
    if (!node) return;

    if (node->is_leaf) {
        free(node->values);
    } else {
        /* Recursively free children */
        for (int i = 0; i <= node->num_keys; i++) {
            node_free(node->children[i]);
        }
        free(node->children);
    }
    free(node->keys);
    free(node);
}

/* ================================================================
 * Internal Helpers — Binary Search
 * ================================================================ */

/*
 * Find the index of the first key >= target in the node's key array.
 * Returns num_keys if all keys are less than target.
 */
static int key_lower_bound(btree_node_t *node, uint32_t target) {
    int lo = 0, hi = node->num_keys;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (node->keys[mid] < target) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/*
 * Find exact key position in a leaf. Returns index or -1 if not found.
 */
static int key_find_exact(btree_node_t *leaf, uint32_t key) {
    int pos = key_lower_bound(leaf, key);
    if (pos < leaf->num_keys && leaf->keys[pos] == key) {
        return pos;
    }
    return -1;
}

/* ================================================================
 * Internal Helpers — Find Leaf
 * ================================================================ */

/* Traverse from root to the leaf that should contain `key`. */
static btree_node_t *find_leaf(btree_t *tree, uint32_t key) {
    if (!tree || !tree->root) return NULL;

    btree_node_t *node = tree->root;
    while (!node->is_leaf) {
        int i = key_lower_bound(node, key);
        /*
         * For internal nodes: if key < keys[i], go to children[i].
         * If key >= keys[i], go to children[i+1]... but lower_bound
         * gives us the first key >= target, so:
         *   - if keys[i] == key, go to children[i+1] (right subtree has key)
         *   - if keys[i] > key, go to children[i]
         *
         * Actually for B+ Tree: child[i] contains keys < keys[i],
         * child[i+1] contains keys >= keys[i]. So:
         */
        if (i < node->num_keys && node->keys[i] == key) {
            /* Exact match — go right (key is in the right subtree in B+ Tree) */
            node = node->children[i + 1];
        } else {
            node = node->children[i];
        }
    }
    return node;
}

/* ================================================================
 * Split Result — used during insertion
 * ================================================================ */

typedef struct {
    int             did_split;
    uint32_t        promoted_key;   /* Key pushed up to parent */
    btree_node_t   *new_node;       /* Right half after split  */
} split_result_t;

/* ================================================================
 * Internal Helpers — Split Nodes
 * ================================================================ */

/*
 * Split a full leaf node. The leaf currently has (order - 1) keys
 * (which is the max). We split into two halves:
 *   - Left (original): keys[0..mid-1]
 *   - Right (new):     keys[mid..order-2]
 *   - Promoted key:    new_node->keys[0] (copied up, NOT removed from leaf)
 */
static split_result_t split_leaf(btree_t *tree, btree_node_t *leaf) {
    split_result_t result = {0, 0, NULL};
    int order = tree->order;
    int total = leaf->num_keys;  /* Should be order - 1 */
    int mid   = total / 2;

    btree_node_t *new_leaf = node_create(order, 1);
    if (!new_leaf) return result;

    /* Move upper half to new leaf */
    int new_count = total - mid;
    memcpy(new_leaf->keys,   &leaf->keys[mid],   new_count * sizeof(uint32_t));
    memcpy(new_leaf->values, &leaf->values[mid],  new_count * sizeof(record_ptr_t));
    new_leaf->num_keys = new_count;

    /* Update original leaf */
    leaf->num_keys = mid;

    /* Link leaves */
    new_leaf->next = leaf->next;
    leaf->next     = new_leaf;

    /* Promote the first key of the new leaf */
    result.did_split    = 1;
    result.promoted_key = new_leaf->keys[0];
    result.new_node     = new_leaf;

    return result;
}

/*
 * Split a full internal node. The node currently has (order - 1) keys.
 *   - Left (original): keys[0..mid-1], children[0..mid]
 *   - Promoted key:    keys[mid] (REMOVED from both halves — goes to parent)
 *   - Right (new):     keys[mid+1..order-2], children[mid+1..order-1]
 */
static split_result_t split_internal(btree_t *tree, btree_node_t *node) {
    split_result_t result = {0, 0, NULL};
    int order = tree->order;
    int total = node->num_keys;
    int mid   = total / 2;

    btree_node_t *new_node = node_create(order, 0);
    if (!new_node) return result;

    /* The middle key is promoted — not stored in either node */
    uint32_t promoted = node->keys[mid];

    /* Move keys[mid+1..] and children[mid+1..] to new node */
    int new_key_count = total - mid - 1;
    memcpy(new_node->keys, &node->keys[mid + 1], new_key_count * sizeof(uint32_t));
    memcpy(new_node->children, &node->children[mid + 1], (new_key_count + 1) * sizeof(btree_node_t *));
    new_node->num_keys = new_key_count;

    /* Shrink original */
    node->num_keys = mid;

    result.did_split    = 1;
    result.promoted_key = promoted;
    result.new_node     = new_node;

    return result;
}

/* ================================================================
 * Insert — Recursive Implementation
 * ================================================================ */

static split_result_t insert_recursive(btree_t *tree, btree_node_t *node,
                                        uint32_t key, record_ptr_t value,
                                        int *error) {
    split_result_t no_split = {0, 0, NULL};

    if (node->is_leaf) {
        /* === LEAF INSERT === */

        /* Check for duplicate */
        int pos = key_lower_bound(node, key);
        if (pos < node->num_keys && node->keys[pos] == key) {
            *error = -1;  /* Duplicate key */
            return no_split;
        }

        /* Shift keys and values right to make room */
        for (int i = node->num_keys; i > pos; i--) {
            node->keys[i]   = node->keys[i - 1];
            node->values[i] = node->values[i - 1];
        }
        node->keys[pos]   = key;
        node->values[pos] = value;
        node->num_keys++;

        /* Check if leaf needs splitting */
        if (node->num_keys >= tree->order - 1) {
            return split_leaf(tree, node);
        }

        return no_split;

    } else {
        /* === INTERNAL NODE — descend to correct child === */

        int i = key_lower_bound(node, key);
        if (i < node->num_keys && node->keys[i] == key) {
            i++;  /* Go to right child if exact match */
        }

        split_result_t child_result = insert_recursive(tree, node->children[i],
                                                        key, value, error);
        if (*error != 0 || !child_result.did_split) {
            return no_split;
        }

        /* Child split — insert promoted key and new child into this node */
        /* Find position for promoted key */
        int pos = key_lower_bound(node, child_result.promoted_key);

        /* Shift keys and children right */
        for (int j = node->num_keys; j > pos; j--) {
            node->keys[j] = node->keys[j - 1];
            node->children[j + 1] = node->children[j];
        }
        node->keys[pos]         = child_result.promoted_key;
        node->children[pos + 1] = child_result.new_node;
        node->num_keys++;

        /* Check if this node needs splitting */
        if (node->num_keys >= tree->order - 1) {
            return split_internal(tree, node);
        }

        return no_split;
    }
}

/* ================================================================
 * Public API — Lifecycle
 * ================================================================ */

btree_t *btree_create(const char *filename, int order) {
    if (!filename || order < 3) {
        etp_log(LOG_ERROR, "btree_create: invalid args (order must be >= 3)");
        return NULL;
    }

    btree_t *tree = calloc(1, sizeof(btree_t));
    if (!tree) {
        etp_log(LOG_ERROR, "btree_create: out of memory");
        return NULL;
    }

    etp_strlcpy(tree->filename, filename, sizeof(tree->filename));
    tree->order = order;
    tree->size  = 0;
    tree->dirty = 1;

    /* Create empty root (a leaf node) */
    tree->root = node_create(order, 1);
    if (!tree->root) {
        free(tree);
        return NULL;
    }

    /* Save initial empty tree to disk */
    if (btree_save(tree) != 0) {
        etp_log(LOG_WARN, "btree_create: failed to save initial index to '%s'", filename);
    }

    etp_log(LOG_INFO, "btree: created new index '%s' (order=%d)", filename, order);
    return tree;
}

btree_t *btree_open(const char *filename) {
    if (!filename) return NULL;

    btree_t *tree = calloc(1, sizeof(btree_t));
    if (!tree) return NULL;

    etp_strlcpy(tree->filename, filename, sizeof(tree->filename));
    tree->dirty = 0;

    /* Load from disk */
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        etp_log(LOG_ERROR, "btree_open: cannot open '%s'", filename);
        free(tree);
        return NULL;
    }

    /* Read header */
    uint32_t magic, order, size, has_root;
    if (fread(&magic, 4, 1, fp) != 1 ||
        fread(&order, 4, 1, fp) != 1 ||
        fread(&size,  4, 1, fp) != 1 ||
        fread(&has_root, 4, 1, fp) != 1) {
        etp_log(LOG_ERROR, "btree_open: failed to read header from '%s'", filename);
        fclose(fp);
        free(tree);
        return NULL;
    }

    if (magic != BTREE_MAGIC) {
        etp_log(LOG_ERROR, "btree_open: invalid magic in '%s' (got 0x%08x)", filename, magic);
        fclose(fp);
        free(tree);
        return NULL;
    }

    tree->order = (int)order;
    tree->size  = size;

    /* Read nodes recursively */
    if (has_root) {
        /* Forward declaration — defined below */
        btree_node_t *read_node(FILE *f, int ord);
        tree->root = read_node(fp, tree->order);
        if (!tree->root) {
            etp_log(LOG_ERROR, "btree_open: failed to deserialize tree from '%s'", filename);
            fclose(fp);
            free(tree);
            return NULL;
        }

        /* Reconstruct leaf `next` pointers by collecting leaves in-order */
        /* We'll do a simple in-order traversal to link them */
        btree_node_t *prev_leaf = NULL;
        void link_leaves(btree_node_t *node) {
            if (!node) return;
            if (node->is_leaf) {
                if (prev_leaf) prev_leaf->next = node;
                prev_leaf = node;
                node->next = NULL;
            } else {
                for (int i = 0; i <= node->num_keys; i++) {
                    link_leaves(node->children[i]);
                }
            }
        }
        link_leaves(tree->root);

    } else {
        tree->root = node_create(tree->order, 1);
    }

    fclose(fp);
    etp_log(LOG_INFO, "btree: opened index '%s' (order=%d, size=%u)", filename, tree->order, tree->size);
    return tree;
}

void btree_close(btree_t *tree) {
    if (!tree) return;

    if (tree->dirty) {
        btree_save(tree);
    }

    node_free(tree->root);
    free(tree);
}

/* ================================================================
 * Public API — Core Operations
 * ================================================================ */

int btree_insert(btree_t *tree, uint32_t key, record_ptr_t value) {
    if (!tree || !tree->root) return -1;

    int error = 0;
    split_result_t result = insert_recursive(tree, tree->root, key, value, &error);

    if (error != 0) {
        return -1;  /* Duplicate key */
    }

    /* If root split, create a new root */
    if (result.did_split) {
        btree_node_t *new_root = node_create(tree->order, 0);
        if (!new_root) return -1;

        new_root->keys[0]     = result.promoted_key;
        new_root->children[0] = tree->root;
        new_root->children[1] = result.new_node;
        new_root->num_keys    = 1;

        tree->root = new_root;
    }

    tree->size++;
    tree->dirty = 1;
    return 0;
}

int btree_search(btree_t *tree, uint32_t key, record_ptr_t *out) {
    if (!tree || !tree->root) return -1;

    btree_node_t *leaf = find_leaf(tree, key);
    if (!leaf) return -1;

    int pos = key_find_exact(leaf, key);
    if (pos < 0) return -1;

    if (out) {
        *out = leaf->values[pos];
    }
    return 0;
}

int btree_delete(btree_t *tree, uint32_t key) {
    /*
     * Lazy deletion: remove key from leaf without rebalancing.
     * This can leave underflowing nodes, which is acceptable for
     * this project's data volume. Production B+ Trees would merge
     * or redistribute keys from siblings.
     */
    if (!tree || !tree->root) return -1;

    btree_node_t *leaf = find_leaf(tree, key);
    if (!leaf) return -1;

    int pos = key_find_exact(leaf, key);
    if (pos < 0) return -1;

    /* Shift keys and values left to fill the gap */
    for (int i = pos; i < leaf->num_keys - 1; i++) {
        leaf->keys[i]   = leaf->keys[i + 1];
        leaf->values[i] = leaf->values[i + 1];
    }
    leaf->num_keys--;

    tree->size--;
    tree->dirty = 1;
    return 0;
}

int btree_update(btree_t *tree, uint32_t key, record_ptr_t new_value) {
    if (!tree || !tree->root) return -1;

    btree_node_t *leaf = find_leaf(tree, key);
    if (!leaf) return -1;

    int pos = key_find_exact(leaf, key);
    if (pos < 0) return -1;

    leaf->values[pos] = new_value;
    tree->dirty = 1;
    return 0;
}

/* ================================================================
 * Public API — Range Scan
 * ================================================================ */

int btree_range_scan(btree_t *tree, uint32_t low, uint32_t high,
                     record_ptr_t *results, int max_results) {
    if (!tree || !tree->root || !results || max_results <= 0) return 0;
    if (low > high) return 0;

    /* Find the leaf that would contain `low` */
    btree_node_t *leaf = find_leaf(tree, low);
    int count = 0;

    while (leaf && count < max_results) {
        for (int i = 0; i < leaf->num_keys && count < max_results; i++) {
            if (leaf->keys[i] > high) {
                return count;  /* Past upper bound — done */
            }
            if (leaf->keys[i] >= low) {
                results[count++] = leaf->values[i];
            }
        }
        leaf = leaf->next;  /* Follow leaf chain */
    }

    return count;
}

/* ================================================================
 * Persistence — Serialization
 * ================================================================ */

/*
 * Write a single node to file (pre-order traversal).
 * Format per node:
 *   [is_leaf: 4] [num_keys: 4] [keys: num_keys * 4]
 *   if leaf:     [values: num_keys * sizeof(record_ptr_t)]
 *   if internal: [children written recursively]
 */
static int write_node(FILE *fp, btree_node_t *node) {
    if (!node) return -1;

    uint32_t is_leaf  = (uint32_t)node->is_leaf;
    uint32_t num_keys = (uint32_t)node->num_keys;

    if (fwrite(&is_leaf,  4, 1, fp) != 1) return -1;
    if (fwrite(&num_keys, 4, 1, fp) != 1) return -1;

    /* Write keys */
    if (num_keys > 0) {
        if (fwrite(node->keys, sizeof(uint32_t), num_keys, fp) != (size_t)num_keys) return -1;
    }

    if (node->is_leaf) {
        /* Write values */
        if (num_keys > 0) {
            if (fwrite(node->values, sizeof(record_ptr_t), num_keys, fp) != (size_t)num_keys) return -1;
        }
    } else {
        /* Write children recursively (num_keys + 1 children) */
        for (int i = 0; i <= node->num_keys; i++) {
            if (write_node(fp, node->children[i]) != 0) return -1;
        }
    }

    return 0;
}

/*
 * Read a single node from file (pre-order traversal).
 * Allocates the node and its children recursively.
 */
btree_node_t *read_node(FILE *fp, int order) {
    uint32_t is_leaf, num_keys;

    if (fread(&is_leaf,  4, 1, fp) != 1) return NULL;
    if (fread(&num_keys, 4, 1, fp) != 1) return NULL;

    btree_node_t *node = node_create(order, (int)is_leaf);
    if (!node) return NULL;

    node->num_keys = (int)num_keys;

    /* Read keys */
    if (num_keys > 0) {
        if (fread(node->keys, sizeof(uint32_t), num_keys, fp) != (size_t)num_keys) {
            node_free(node);
            return NULL;
        }
    }

    if (node->is_leaf) {
        /* Read values */
        if (num_keys > 0) {
            if (fread(node->values, sizeof(record_ptr_t), num_keys, fp) != (size_t)num_keys) {
                node_free(node);
                return NULL;
            }
        }
    } else {
        /* Read children recursively */
        for (uint32_t i = 0; i <= num_keys; i++) {
            node->children[i] = read_node(fp, order);
            if (!node->children[i]) {
                node_free(node);
                return NULL;
            }
        }
    }

    return node;
}

int btree_save(btree_t *tree) {
    if (!tree) return -1;

    FILE *fp = fopen(tree->filename, "wb");
    if (!fp) {
        etp_log(LOG_ERROR, "btree_save: cannot open '%s' for writing", tree->filename);
        return -1;
    }

    /* Write header */
    uint32_t magic    = BTREE_MAGIC;
    uint32_t order    = (uint32_t)tree->order;
    uint32_t size     = tree->size;
    uint32_t has_root = (tree->root != NULL) ? 1 : 0;

    fwrite(&magic,    4, 1, fp);
    fwrite(&order,    4, 1, fp);
    fwrite(&size,     4, 1, fp);
    fwrite(&has_root, 4, 1, fp);

    /* Write nodes */
    if (tree->root) {
        if (write_node(fp, tree->root) != 0) {
            etp_log(LOG_ERROR, "btree_save: failed to serialize tree to '%s'", tree->filename);
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    tree->dirty = 0;

    etp_log(LOG_DEBUG, "btree: saved index '%s' (size=%u)", tree->filename, tree->size);
    return 0;
}

/* ================================================================
 * Utility
 * ================================================================ */

uint32_t btree_size(btree_t *tree) {
    return tree ? tree->size : 0;
}

/* Recursive print helper */
static void print_node(btree_node_t *node, int depth) {
    if (!node) return;

    /* Indent */
    for (int i = 0; i < depth; i++) fprintf(stderr, "  ");

    if (node->is_leaf) {
        fprintf(stderr, "[LEAF keys=%d] ", node->num_keys);
        for (int i = 0; i < node->num_keys; i++) {
            fprintf(stderr, "%u", node->keys[i]);
            if (i < node->num_keys - 1) fprintf(stderr, ",");
        }
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "[INTERNAL keys=%d] ", node->num_keys);
        for (int i = 0; i < node->num_keys; i++) {
            fprintf(stderr, "%u", node->keys[i]);
            if (i < node->num_keys - 1) fprintf(stderr, ",");
        }
        fprintf(stderr, "\n");

        for (int i = 0; i <= node->num_keys; i++) {
            print_node(node->children[i], depth + 1);
        }
    }
}

void btree_print(btree_t *tree) {
    if (!tree) {
        fprintf(stderr, "(null tree)\n");
        return;
    }
    fprintf(stderr, "=== B+ Tree (order=%d, size=%u) ===\n", tree->order, tree->size);
    print_node(tree->root, 0);
    fprintf(stderr, "===================================\n");
}
