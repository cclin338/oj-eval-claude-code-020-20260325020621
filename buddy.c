#include "buddy.h"
#include <stdlib.h>
#define NULL ((void *)0)

#define MAX_RANK 16
#define PAGE_SIZE 4096  // 4KB

// Free list structure
typedef struct free_block {
    struct free_block *next;
} free_block_t;

// Allocated block structure
typedef struct alloc_block {
    struct alloc_block *next;
    void *addr;
    int rank;
} alloc_block_t;

// Global state
static void *base_addr = NULL;
static int total_pages = 0;
static free_block_t *free_list[MAX_RANK + 1];  // Index 1-16
static alloc_block_t *alloc_list = NULL;

// Helper function to check if address is valid
static int is_valid_addr(void *addr) {
    if (base_addr == NULL || addr == NULL) return 0;
    unsigned long offset = (unsigned long)addr - (unsigned long)base_addr;
    return (offset < total_pages * PAGE_SIZE) && (offset % PAGE_SIZE == 0);
}

// Helper function to get buddy address
static void *get_buddy(void *addr, int rank) {
    unsigned long offset = (unsigned long)addr - (unsigned long)base_addr;
    unsigned long block_size = PAGE_SIZE << (rank - 1);
    unsigned long buddy_offset = offset ^ block_size;
    return (void *)((unsigned long)base_addr + buddy_offset);
}

// Helper function to check if a block is in free list
static int is_in_free_list(void *addr, int rank) {
    free_block_t *block = free_list[rank];
    while (block) {
        if ((void *)block == addr) return 1;
        block = block->next;
    }
    return 0;
}

// Helper function to remove from free list
static void remove_from_free_list(void *addr, int rank) {
    free_block_t **curr = &free_list[rank];
    while (*curr) {
        if ((void *)*curr == addr) {
            *curr = (*curr)->next;
            return;
        }
        curr = &(*curr)->next;
    }
}

// Helper function to add to free list
static void add_to_free_list(void *addr, int rank) {
    free_block_t *block = (free_block_t *)addr;
    block->next = free_list[rank];
    free_list[rank] = block;
}

// Helper function to add to alloc list
static void add_to_alloc_list(void *addr, int rank) {
    alloc_block_t *block = (alloc_block_t *)malloc(sizeof(alloc_block_t));
    block->addr = addr;
    block->rank = rank;
    block->next = alloc_list;
    alloc_list = block;
}

// Helper function to remove from alloc list
static void remove_from_alloc_list(void *addr) {
    alloc_block_t **curr = &alloc_list;
    while (*curr) {
        if ((*curr)->addr == addr) {
            alloc_block_t *to_remove = *curr;
            *curr = (*curr)->next;
            free(to_remove);
            return;
        }
        curr = &(*curr)->next;
    }
}

// Helper function to find alloc block
static alloc_block_t *find_alloc_block(void *addr) {
    alloc_block_t *curr = alloc_list;
    while (curr) {
        if (curr->addr == addr) return curr;
        curr = curr->next;
    }
    return NULL;
}

// Helper function to get max rank for unallocated address
static int get_max_rank_for_addr(void *addr) {
    if (!is_valid_addr(addr)) return -EINVAL;

    unsigned long offset = (unsigned long)addr - (unsigned long)base_addr;
    int max_rank = 1;

    // Find the largest rank where this address is the start of a block
    for (int rank = 1; rank <= MAX_RANK; rank++) {
        unsigned long block_size = PAGE_SIZE << (rank - 1);
        if (offset % block_size == 0) {
            // Check if the entire block fits in memory
            unsigned long end_offset = offset + block_size;
            if (end_offset <= total_pages * PAGE_SIZE) {
                max_rank = rank;
            }
        }
    }

    return max_rank;
}

int init_page(void *p, int pgcount) {
    if (p == NULL || pgcount <= 0) return -EINVAL;

    base_addr = p;
    total_pages = pgcount;

    // Initialize free lists
    for (int i = 1; i <= MAX_RANK; i++) {
        free_list[i] = NULL;
    }

    // Initialize alloc list
    alloc_list = NULL;

    // Add all memory to the largest possible rank
    int max_rank = 0;
    unsigned long total_size = pgcount * PAGE_SIZE;

    // Find the largest rank that can fit all memory
    for (int rank = 1; rank <= MAX_RANK; rank++) {
        unsigned long block_size = PAGE_SIZE << (rank - 1);
        if (block_size <= total_size && (total_size % block_size == 0 || block_size * 2 > total_size)) {
            max_rank = rank;
        }
    }

    if (max_rank > 0) {
        add_to_free_list(base_addr, max_rank);
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) return ERR_PTR(-EINVAL);

    // Try to find a free block of the requested rank
    if (free_list[rank] != NULL) {
        void *addr = (void *)free_list[rank];
        remove_from_free_list(addr, rank);
        add_to_alloc_list(addr, rank);
        return addr;
    }

    // Try to split a larger block
    for (int curr_rank = rank + 1; curr_rank <= MAX_RANK; curr_rank++) {
        if (free_list[curr_rank] != NULL) {
            void *block = (void *)free_list[curr_rank];
            remove_from_free_list(block, curr_rank);

            // Split the block down to the requested rank
            for (int split_rank = curr_rank - 1; split_rank >= rank; split_rank--) {
                void *buddy = get_buddy(block, split_rank);
                add_to_free_list(buddy, split_rank);
            }

            add_to_alloc_list(block, rank);
            return block;
        }
    }

    return ERR_PTR(-ENOSPC);
}

int return_pages(void *p) {
    if (!is_valid_addr(p)) return -EINVAL;

    // Find the allocated block
    alloc_block_t *alloc_block = find_alloc_block(p);
    if (!alloc_block) return -EINVAL;

    int rank = alloc_block->rank;
    remove_from_alloc_list(p);

    // Try to merge with buddy
    void *curr_block = p;
    int curr_rank = rank;

    while (curr_rank < MAX_RANK) {
        void *buddy = get_buddy(curr_block, curr_rank);

        // Check if buddy is free
        if (!is_in_free_list(buddy, curr_rank)) break;

        // Remove buddy from free list
        remove_from_free_list(buddy, curr_rank);

        // Merge to get the larger block
        if (curr_block > buddy) {
            curr_block = buddy;
        }
        curr_rank++;
    }

    // Add the merged block to free list
    add_to_free_list(curr_block, curr_rank);

    return OK;
}

int query_ranks(void *p) {
    if (!is_valid_addr(p)) return -EINVAL;

    // Check if this is an allocated block
    alloc_block_t *alloc_block = find_alloc_block(p);
    if (alloc_block) {
        return alloc_block->rank;
    }

    // For unallocated blocks, return the maximum rank
    return get_max_rank_for_addr(p);
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) return -EINVAL;

    int count = 0;
    free_block_t *block = free_list[rank];
    while (block) {
        count++;
        block = block->next;
    }

    return count;
}
