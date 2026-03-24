#include "buddy.h"
#include <stdlib.h>
#include <string.h>
#define NULL ((void *)0)

#define MAX_RANK 16
#define PAGE_SIZE 4096  // 4KB
#define MAX_PAGES (128 * 1024 / 4)  // Maximum pages in test

// Free list structure
typedef struct free_block {
    struct free_block *next;
} free_block_t;

// Global state
static void *base_addr = NULL;
static int total_pages = 0;
static free_block_t *free_list[MAX_RANK + 1];  // Index 1-16

// Track allocated blocks using a simple array for O(1) lookup
static char *alloc_map = NULL;  // 1 if allocated, 0 if free
static int *alloc_rank = NULL;  // Rank of allocated block

// Helper function to check if address is valid
static int is_valid_addr(void *addr) {
    if (base_addr == NULL || addr == NULL) return 0;
    unsigned long offset = (unsigned long)addr - (unsigned long)base_addr;
    return (offset < total_pages * PAGE_SIZE) && (offset % PAGE_SIZE == 0);
}

// Helper function to get page index from address
static int addr_to_page_idx(void *addr) {
    return ((unsigned long)addr - (unsigned long)base_addr) / PAGE_SIZE;
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

// Helper function to mark block as allocated
static void mark_allocated(void *addr, int rank) {
    int page_idx = addr_to_page_idx(addr);
    int num_pages = 1 << (rank - 1);
    for (int i = 0; i < num_pages; i++) {
        alloc_map[page_idx + i] = 1;
        alloc_rank[page_idx + i] = rank;
    }
}

// Helper function to mark block as free
static void mark_free(void *addr, int rank) {
    int page_idx = addr_to_page_idx(addr);
    int num_pages = 1 << (rank - 1);
    for (int i = 0; i < num_pages; i++) {
        alloc_map[page_idx + i] = 0;
        alloc_rank[page_idx + i] = 0;
    }
}

// Helper function to check if block is allocated
static int is_allocated(void *addr) {
    int page_idx = addr_to_page_idx(addr);
    return alloc_map[page_idx];
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

    // Initialize allocation tracking arrays
    alloc_map = (char *)calloc(pgcount, sizeof(char));
    alloc_rank = (int *)calloc(pgcount, sizeof(int));

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
        mark_allocated(addr, rank);
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

            mark_allocated(block, rank);
            return block;
        }
    }

    return ERR_PTR(-ENOSPC);
}

int return_pages(void *p) {
    if (!is_valid_addr(p)) return -EINVAL;

    // Check if block is allocated
    if (!is_allocated(p)) return -EINVAL;

    int page_idx = addr_to_page_idx(p);
    int rank = alloc_rank[page_idx];

    mark_free(p, rank);

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
    if (is_allocated(p)) {
        int page_idx = addr_to_page_idx(p);
        return alloc_rank[page_idx];
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
