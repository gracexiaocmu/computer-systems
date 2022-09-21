/**
 * @file mm.c
 * @brief A 64-bit struct-based implicit free list memory allocator
 *
 * 15-213: Introduction to Computer Systems
 *
 * TODO: insert your documentation here. :)
 * For checkpoint: First change the implementation to explicit allocator
 * and then to segregated allocator.
 *
 *************************************************************************
 *
 * ADVICE FOR STUDENTS.
 * - Step 0: Please read the writeup!
 * - Step 1: Write your heap checker.
 * - Step 2: Write contracts / debugging assert statements.
 * - Good luck, and have fun!
 *
 *************************************************************************
 *
 * @author Yuxuan Xiao <yuxuanx@andrew.cmu.edu>
 */

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memlib.h"
#include "mm.h"

/* Do not change the following! */

#ifdef DRIVER
/* create aliases for driver tests */
#define malloc mm_malloc
#define free mm_free
#define realloc mm_realloc
#define calloc mm_calloc
#define memset mem_memset
#define memcpy mem_memcpy
#endif /* def DRIVER */

/* You can change anything from here onward */

/*
 *****************************************************************************
 * If DEBUG is defined (such as when running mdriver-dbg), these macros      *
 * are enabled. You can use them to print debugging output and to check      *
 * contracts only in debug mode.                                             *
 *                                                                           *
 * Only debugging macros with names beginning "dbg_" are allowed.            *
 * You may not define any other macros having arguments.                     *
 *****************************************************************************
 */
#ifdef DEBUG
/* When DEBUG is defined, these form aliases to useful functions */
#define dbg_printf(...) printf(__VA_ARGS__)
#define dbg_requires(expr) assert(expr)
#define dbg_assert(expr) assert(expr)
#define dbg_ensures(expr) assert(expr)
#define dbg_printheap(...) print_heap(__VA_ARGS__)
#else
/* When DEBUG is not defined, no code gets generated for these */
/* The sizeof() hack is used to avoid "unused variable" warnings */
#define dbg_printf(...) (sizeof(__VA_ARGS__), -1)
#define dbg_requires(expr) (sizeof(expr), 1)
#define dbg_assert(expr) (sizeof(expr), 1)
#define dbg_ensures(expr) (sizeof(expr), 1)
#define dbg_printheap(...) ((void)sizeof(__VA_ARGS__))
#endif

/* Basic constants */

typedef uint64_t word_t;

/** @brief Word and header size (bytes) */
static const size_t wsize = sizeof(word_t);

/** @brief Double word size (bytes) */
static const size_t dsize = 2 * wsize;

/** @brief Minimum block size (bytes) */
static const size_t min_block_size = dsize;

/**
 * @brief Minimum block size that can be allocated
 */
static const size_t chunksize = (1 << 12);

/**
 * @brief Mask that can extract alloc bit from header
 */
static const word_t alloc_mask = 0x1;

/**
 * @brief Mask that can extract prev_alloc bit from header
 */
static const word_t prev_alloc_mask = 0x2;

/**
 * @brief Mask that can extract mini block bit from header
 */
static const word_t mini_mask = 0x4;

/**
 * @brief Mask that can extract size from header
 */
static const word_t size_mask = ~(word_t)0xF;

/**
 * @brief Mask that can extract next pointer from miniblock header
 */
static const word_t next_mask = ~(word_t)0x7;

/** @brief Represents the header and payload of one block in the heap */
typedef struct block {
    /** @brief Header contains size + allocation flag */
    word_t header;

    /**
     * @brief A union that casts between a struct and a pointer to payload.
     *
     * TODO: feel free to delete this comment once you've read it carefully.
     * We don't know what the size of the payload will be, so we will declare
     * it as a zero-length array, which is a GCC compiler extension. This will
     * allow us to obtain a pointer to the start of the payload.
     *
     * WARNING: A zero-length array must be the last element in a struct, so
     * there should not be any struct fields after it. For this lab, we will
     * allow you to include a zero-length array in a union, as long as the
     * union is the last field in its containing struct. However, this is
     * compiler-specific behavior and should be avoided in general.
     *
     * WARNING: DO NOT cast this pointer to/from other types! Instead, you
     * should use a union to alias this zero-length array with another struct,
     * in order to store additional types of data in the payload memory.
     */
    union {
        struct {
            /** @brief pointer to next free block*/
            struct block *next;
            /** @brief pointer to previous free block*/
            struct block *prev;
        };
        /** @brief payload of allocated block*/
        char payload[0];
    };

} block_t;

/* Global variables */

/** @brief Pointer to first block in the heap */
static block_t *heap_start = NULL;

/** @brief Global variable that stores the number of buckets in seglist */
const size_t BUCKET_NUM = 15;

/** @brief Pointer to the head of the explicit list */
block_t *seglist[BUCKET_NUM];

/*
 *****************************************************************************
 * The functions below are short wrapper functions to perform                *
 * bit manipulation, pointer arithmetic, and other helper operations.        *
 *                                                                           *
 * We've given you the function header comments for the functions below      *
 * to help you understand how this baseline code works.                      *
 *                                                                           *
 * Note that these function header comments are short since the functions    *
 * they are describing are short as well; you will need to provide           *
 * adequate details for the functions that you write yourself!               *
 *****************************************************************************
 */

/*
 * ---------------------------------------------------------------------------
 *                        BEGIN SHORT HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Returns the maximum of two integers.
 * @param[in] x
 * @param[in] y
 * @return `x` if `x > y`, and `y` otherwise.
 */
static size_t max(size_t x, size_t y) {
    return (x > y) ? x : y;
}

/**
 * @brief Rounds `size` up to next multiple of n
 * @param[in] size
 * @param[in] n
 * @return The size after rounding up
 */
static size_t round_up(size_t size, size_t n) {
    return n * ((size + (n - 1)) / n);
}

/**
 * @brief Packs the `size`, `alloc`, and 'prev_alloc of a block
 *          into a word suitable for use as a packed value.
 *
 * Packed values are used for both headers and footers.
 *
 * The allocation status is packed into the lowest bit of the word.
 *
 * @param[in] size The size of the block being represented
 * @param[in] alloc True if the block is allocated
 * @param[in] prev_alloc True if the previous block is allocated
 * @return The packed value
 */
static word_t pack(size_t size, bool alloc, bool prev_alloc) {
    word_t word = size;
    if (alloc) {
        word |= alloc_mask;
    }
    if (prev_alloc) {
        word |= prev_alloc_mask;
    }
    return word;
}

/**
 * @brief Packs the `size`, `alloc`, and 'prev_alloc' of a mini block
 *          into a word suitable for use as a packed value.
 *
 * Packed values are used for both headers and footers.
 *
 * The allocation status is packed into the lowest bit of the word.
 *
 * @param[in] size The size of the block being represented
 * @param[in] alloc True if the block is allocated
 * @param[in] prev_alloc True if the previous block is allocated
 *
 * @return The packed value
 */
static word_t mini_pack(word_t header, bool alloc, bool prev_alloc) {
    word_t word = header | mini_mask;
    if (alloc) {
        word |= alloc_mask;
    }
    if (prev_alloc) {
        word |= prev_alloc_mask;
    }
    return word;
}

/**
 * @brief Extracts the mini bit represented in a packed word.
 *
 * This function simply takes the third to last bit of the word.
 *
 * @param[in] word
 * @return The mini bit of the block represented by the word
 */
static bool extract_mini(word_t word) {
    return (bool)(word & mini_mask);
}

/**
 * @brief Extracts the mini bit of a block from its header.
 * @param[in] block
 * @return The mini bit of the block
 */
static bool get_mini(block_t *block) {
    return extract_mini(block->header);
}

/**
 * @brief Extracts the size represented in a packed word.
 *
 * This function simply clears the lowest 4 bits of the word, as the heap
 * is 16-byte aligned.
 *
 * @param[in] word
 * @return The size of the block represented by the word
 */
static size_t extract_size(word_t word) {
    return (word & size_mask);
}

/**
 * @brief Extracts the size of a block from its header.
 * @param[in] block
 * @return The size of the block
 */
static size_t get_size(block_t *block) {
    if (get_mini(block)) {
        return dsize;
    }
    return extract_size(block->header);
}

/**
 * @brief Given a payload pointer, returns a pointer to the corresponding
 *        block.
 * @param[in] bp A pointer to a block's payload
 * @return The corresponding block
 */
static block_t *payload_to_header(void *bp) {
    return (block_t *)((char *)bp - offsetof(block_t, payload));
}

/**
 * @brief Given a block pointer, returns a pointer to the corresponding
 *        payload.
 * @param[in] block
 * @return A pointer to the block's payload
 * @pre The block must be a valid block, not a boundary tag.
 */
static void *header_to_payload(block_t *block) {
    dbg_requires(get_size(block) != 0);
    return (void *)(block->payload);
}

/**
 * @brief Given a block pointer, returns a pointer to the corresponding
 *        footer.
 * @param[in] block
 * @return A pointer to the block's footer
 * @pre The block must be a valid block, not a boundary tag.
 */
static word_t *header_to_footer(block_t *block) {
    dbg_requires(get_size(block) != 0 &&
                 "Called header_to_footer on the epilogue block");
    return (word_t *)(block->payload + get_size(block) - dsize);
}

/**
 * @brief Given a block footer, returns a pointer to the corresponding
 *        header.
 * @param[in] footer A pointer to the block's footer
 * @return A pointer to the start of the block
 * @pre The footer must be the footer of a valid block, not a boundary tag.
 */
static block_t *footer_to_header(word_t *footer) {
    size_t size = extract_size(*footer);
    dbg_assert(size != 0 && "Called footer_to_header on the prologue block");
    return (block_t *)((char *)footer + wsize - size);
}

/**
 * @brief Returns the payload size of a given block.
 *
 * The payload size is equal to the entire block size minus the sizes of the
 * block's header and footer.
 *
 * @param[in] block
 * @return The size of the block's payload
 */
static size_t get_payload_size(block_t *block) {
    if (get_mini(block)) {
        return wsize;
    }
    size_t asize = get_size(block);
    return asize - wsize;
}

/**
 * @brief Returns the allocation status of a given header value.
 *
 * This is based on the lowest bit of the header value.
 *
 * @param[in] word
 * @return The allocation status correpsonding to the word
 */
static bool extract_alloc(word_t word) {
    return (bool)(word & alloc_mask);
}

/**
 * @brief Returns the allocation status of the previous
 * block of a given header value.
 *
 * This is based on the lowest bit of the header value in previous
 * block.
 *
 * @param[in] word
 * @return The allocation status correpsonding to the word
 */
static bool extract_prev_alloc(word_t word) {
    return (bool)(word & prev_alloc_mask);
}

/**
 * @brief Returns the allocation status of a block, based on its header.
 * @param[in] block
 * @return The allocation status of the block
 */
static bool get_alloc(block_t *block) {
    return extract_alloc(block->header);
}

/**
 * @brief Returns the allocation status of the previous block,
 * based on its header.
 * @param[in] block
 * @return The allocation status of the previous block
 */
static bool get_prev_alloc(block_t *block) {
    return extract_prev_alloc(block->header);
}

/**
 * @brief Returns the pointer to the next block of a mini block
 *  given header value.
 *
 * This function simply clears the lowest 3 bits of the word, as the
 * address of the block is 16-byte aligned.
 *
 * @param[in] word
 * @return The allocation status correpsonding to the word
 */
static word_t extract_header(word_t word) {
    return (word_t)(word & next_mask);
}

/**
 * @brief Extracts the pointer to the next block on list
 * from its header.
 * @param[in] block
 * @return The address of the next block in list
 */
static word_t mini_get_header(block_t *block) {
    return extract_header(block->header);
}

/**
 * @brief Extracts the pointer to the previous block on list
 * from its header.
 * @param[in] block
 * @return The address of the previous block in list
 */
static word_t mini_get_next(block_t *block) {
    return extract_header((word_t)(block->next));
}

/**
 * @brief Writes an epilogue header at the given address.
 *
 * The epilogue header has size 0, and is marked as allocated.
 *
 * @param[out] block The location to write the epilogue header
 */
static void write_epilogue(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires((char *)block == mem_heap_hi() - 7);
    block->header = pack(0, true, false);
}

/**
 * @brief Finds the next consecutive block on the heap.
 *
 * This function accesses the next block in the "implicit list" of the heap
 * by adding the size of the block.
 *
 * @param[in] block A block in the heap
 * @return The next consecutive block on the heap
 * @pre The block is not the epilogue
 */
static block_t *find_next(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(get_size(block) != 0 &&
                 "Called find_next on the last block in the heap");
    if (get_mini(block)) {
        return (block_t *)((char *)block + dsize);
    }
    return (block_t *)((char *)block + get_size(block));
}

/**
 * @brief Writes a block requested from memory to be stored in heap.
 *
 * This function is only called by extend_heap and only modifies the
 * header and the footer of the block. It doesn't update the next
 * block (which should be the epilogue in this case) as in the
 * write_block() function.
 *
 * @param[out] block A block in the heap
 * @param[in] size Size of newly requested space
 * @param[in] alloc True if the current block is allocated
 * @param[in] prev_alloc True if the previous block is allocated
 * @param[in] mini True if this block is miniblock (always false)
 * @pre The block is not NULL
 */
static void extend_write(block_t *block, size_t size, bool alloc,
                         bool prev_alloc, bool mini) {
    dbg_requires(block != NULL);
    block->header = pack(size, alloc, prev_alloc);
    word_t *footerp = header_to_footer(block);
    *footerp = pack(size, alloc, prev_alloc);
}

/**
 * @brief Writes a block starting at the given address.
 *
 * This function writes both a header and footer (if the block is not
 * allocated), where the location of the
 * footer is computed in relation to the header.
 * It also updates the prev_alloc bit of the next block in heap.
 *
 * @pre The block must not be NULL
 * @pre The size must be greater than 0
 *
 * @param[out] block The location to begin writing the block header
 * @param[in] size The size of the new block
 * @param[in] alloc The allocation status of the new block
 * @param[in] prev_alloc The allocation status of the previous block in heap
 * @param[in] mini True if the current block should be mini
 */
static void write_block(block_t *block, size_t size, bool alloc,
                        bool prev_alloc, bool mini) {
    dbg_requires(block != NULL);
    dbg_requires(size > 0);

    // Current block is not mini block
    if (!mini) {
        block->header = pack(size, alloc, prev_alloc);
        if (!alloc) {
            word_t *footerp = header_to_footer(block);
            *footerp = pack(size, alloc, prev_alloc);
        }
        block_t *next = find_next(block);
        size_t next_size = get_size(next);
        bool next_alloc = get_alloc(next);
        bool next_mini = get_mini(next);
        if (!next_mini) {
            next->header = pack(next_size, next_alloc, alloc);
            if (!next_alloc) {
                word_t *footerp = header_to_footer(block);
                *footerp = pack(next_size, next_alloc, alloc);
            }
        }
        // Next block in heap is mini block
        else {
            size_t next_header = mini_get_header(next);
            next->header = mini_pack((word_t)next_header, next_alloc, alloc);
            // When next block is not allocated, modify next (payload)
            if (!next_alloc) {
                block_t *next_next = (block_t *)mini_get_next(next);
                next->next =
                    (block_t *)mini_pack((word_t)next_next, next_alloc, alloc);
            }
        }
    }
    // Current block is mini block
    else {
        // Updates current block header and next
        word_t header = mini_get_header(block);
        word_t nxt = mini_get_next(block);
        block->header = mini_pack((word_t)header, alloc, prev_alloc);
        block->next = (block_t *)mini_pack((word_t)nxt, alloc, prev_alloc);

        // Updates the prev_alloc bit of the next block
        block_t *next = find_next(block);
        bool next_alloc = get_alloc(next);
        bool next_mini = get_mini(next);
        size_t next_size;
        if (!next_mini) {
            next_size = get_size(next);
            next->header = pack(next_size, next_alloc, alloc);
            if (!next_alloc) {
                word_t *footerp = header_to_footer(block);
                *footerp = pack(next_size, next_alloc, alloc);
            }
        }
        // Next block in heap is mini block
        else {
            size_t next_header = mini_get_header(next);
            next->header = mini_pack((word_t)next_header, next_alloc, alloc);
            if (!next_alloc) {
                block_t *next_next = (block_t *)mini_get_next(next);
                next->next =
                    (block_t *)mini_pack((word_t)next_next, next_alloc, alloc);
            }
        }
    }
}

/**
 * @brief Finds the footer of the previous block on the heap.
 * @param[in] block A block in the heap
 * @return The location of the previous block's footer
 */
static word_t *find_prev_footer(block_t *block) {
    // Compute previous footer position as one word before the header
    return &(block->header) - 1;
}

/**
 * @brief Finds the previous consecutive block on the heap.
 *
 * This is the previous block in the "implicit list" of the heap.
 *
 * If the function is called on the first block in the heap, NULL will be
 * returned, since the first block in the heap has no previous block!
 *
 * The position of the previous block is found by reading the previous
 * block's footer to determine its size, then calculating the start of the
 * previous block based on its size.
 *
 * @param[in] block A block in the heap
 * @return The previous consecutive block in the heap.
 */
static block_t *find_prev(block_t *block) {
    dbg_requires(block != NULL);
    word_t *footerp = find_prev_footer(block);

    // Header is 1 word size ahead if previous block is mini
    if (extract_mini(*footerp)) {
        return (block_t *)((char *)footerp - wsize);
    }

    // Return NULL if called on first block in the heap
    if (extract_size(*footerp) == 0) {
        return NULL;
    }

    return footer_to_header(footerp);
}

/*
 * ---------------------------------------------------------------------------
 *                        END SHORT HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/******** The remaining content below are helper and debug routines ********/

/**
 * @brief Finds the bucket index based on the block size
 * @param[in] size
 * @return The bucket index in seglist of given size
 */
static size_t find_bucket(size_t size) {
    dbg_requires(size > 0);
    if (size <= 16) {
        return 0;
    } else if (size > 16 && size <= 32) {
        return 1;
    } else if (size > 32 && size <= 64) {
        return 2;
    } else if (size > 64 && size <= 128) {
        return 3;
    } else if (size > 128 && size <= 256) {
        return 4;
    } else if (size > 256 && size <= 512) {
        return 5;
    } else if (size > 512 && size <= 1024) {
        return 6;
    } else if (size > 1024 && size <= 2048) {
        return 7;
    } else if (size > 2048 && size <= 4096) {
        return 8;
    } else if (size > 4096 && size <= 8192) {
        return 9;
    } else if (size > 8192 && size <= 16384) {
        return 10;
    } else if (size > 16384 && size <= 32768) {
        return 11;
    } else if (size > 32768 && size <= 65536) {
        return 12;
    } else if (size > 65536 && size <= 131072) {
        return 13;
    } else {
        return 14;
    }
}

/**
 * @brief Prints all the blocks and their informations on heap
 */
static void print_heap() {
    block_t *tmp = heap_start;
    while (get_size(tmp) != 0) {
        block_t *prev;
        if (!get_prev_alloc(tmp)) {
            prev = find_prev(tmp);
        }
        block_t *next = find_next(tmp);
        dbg_printf("size %zu, alloc %d, prevalloc %d, mini %d\n", get_size(tmp),
                   get_alloc(tmp), get_prev_alloc(tmp), get_mini(tmp));
        dbg_printf("block address %zu, block next addr %zu\n", (size_t)tmp,
                   (size_t)next);
        if (!get_prev_alloc(tmp)) {
            dbg_printf("block prev addr %zu\n", (size_t)prev);
        }
        tmp = next;
    }
}

/**
 * @brief Insert a given block into the segregated lists
 * @param[in] block
 */
static void insert(block_t *block) {
    if (block == NULL) {
        dbg_printf("inserting NULL\n");
        return;
    }
    if (!get_mini(block)) {
        size_t bucket = find_bucket(get_size(block));
        // the bucket doesn't have any block
        if (seglist[bucket] == NULL) {
            seglist[bucket] = block;
            block->prev = NULL;
            block->next = NULL;
        } else {
            seglist[bucket]->prev = block;
            block->next = seglist[bucket];
            block->prev = NULL;
            seglist[bucket] = block;
        }
    }
    // The inserted block is mini block
    else {
        bool prev_alloc = get_prev_alloc(block);
        bool alloc = get_alloc(block);
        // The bucket is empty
        if (seglist[0] == NULL) {
            seglist[0] = block;
            block->header = mini_pack((word_t)0, alloc, prev_alloc);
            block->next = (block_t *)mini_pack((word_t)0, alloc, prev_alloc);
        } else {
            block_t *head = seglist[0];
            bool head_alloc = get_alloc(head);
            bool head_prev_alloc = get_prev_alloc(head);
            head->next = (block_t *)mini_pack((word_t)block, head_alloc,
                                              head_prev_alloc);
            block->header = mini_pack((word_t)head, alloc, prev_alloc);
            block->next = (block_t *)mini_pack((word_t)0, alloc, prev_alloc);
            seglist[0] = block;
        }
    }
}

/**
 * @brief Deletes a block from the segregated list
 * @param[in] block
 */
static void delete (block_t *block) {
    if (!get_mini(block)) {
        size_t bucket = find_bucket(get_size(block));
        // block is the first element in the bucket
        if (block == seglist[bucket]) {
            // Only block in this bucket
            if (block->next == NULL) {
                seglist[bucket] = NULL;
            } else {
                seglist[bucket] = block->next;
                block->next->prev = NULL;
            }
            // block is not the first element in the bucket
        } else {
            block->prev->next = block->next;
            // block is not the last element in the bucket
            if (block->next != NULL) {
                block->next->prev = block->prev;
            }
        }
        block->next = NULL;
        block->prev = NULL;
    }
    // The block is mini block
    else {
        // First block in the bucket
        if (block == seglist[0]) {
            // Only block in this bucket
            if (mini_get_header(block) == 0) {
                seglist[0] = NULL;
            } else {
                seglist[0] = (block_t *)mini_get_header(block);
                bool prev_alloc = get_prev_alloc(seglist[0]);
                seglist[0]->next =
                    (block_t *)mini_pack((word_t)0, false, prev_alloc);
            }
        }
        // Not the first block in the bucket
        else {
            block_t *previous = (block_t *)mini_get_next(block);
            bool prev_prev_alloc = get_prev_alloc(previous);
            block_t *next;
            // In the middle of the bucket
            if (mini_get_header(block) != 0) {
                next = (block_t *)mini_get_header(block);
                bool next_prev_alloc = get_prev_alloc(next);
                next->next = (block_t *)mini_pack((word_t)previous, false,
                                                  next_prev_alloc);
                previous->header =
                    mini_pack((word_t)next, false, prev_prev_alloc);
            } else {
                previous->header = mini_pack((word_t)0, false, prev_prev_alloc);
            }
        }
        bool prev_alloc = get_prev_alloc(block);
        block->header = mini_pack((word_t)0, true, prev_alloc);
        block->next = (block_t *)mini_pack((word_t)0, true, prev_alloc);
    }
}

/**
 * @brief Coalesce current block with previous and next blocks on heap
 *
 * This is the previous or next block in the "implicit list" of the heap
 * and is guaranteed to be free.
 *
 * @param[in] block
 * @param[in] prev
 * @param[in] next
 * @return The pointer to the coalesced block
 */
static block_t *coalesce_prev_next(block_t *block, block_t *prev, block_t *next,
                                   size_t size) {
    size_t prev_size = get_size(prev);
    size_t next_size = get_size(next);
    size_t total_size = size + prev_size + next_size;
    bool prev_prev_alloc = get_prev_alloc(prev);
    delete (prev);
    delete (next);
    write_block(prev, total_size, false, prev_prev_alloc, false);
    return prev;
}

/**
 * @brief Coalesce current block with previous block on heap
 *
 * This is the previous block in the "implicit list" of the heap
 * and is guaranteed to be free.
 *
 * @param[in] prev
 * @param[in] next
 * @return The pointer to the coalesced block
 */
static block_t *coalesce_prev(block_t *block, block_t *previous, size_t size) {
    size_t prev_size = get_size(previous);
    size_t total_size = size + prev_size;
    bool prev_prev_alloc = get_prev_alloc(previous);
    delete (previous);
    write_block(previous, total_size, false, prev_prev_alloc, false);
    return previous;
}

/**
 * @brief Coalesce current block with next block on heap
 *
 * This is the next block in the "implicit list" of the heap
 * and is guaranteed to be free.
 *
 * @param[in] block
 * @param[in] next
 * @return The pointer to the coalesced block
 */
static block_t *coalesce_next(block_t *block, block_t *next, size_t size) {
    size_t next_size = get_size(next);
    size_t total_size = size + next_size;
    bool prev_alloc = get_prev_alloc(block);
    delete (next);
    write_block(block, total_size, false, prev_alloc, false);
    return block;
}

/**
 * @brief Checks and coalesce the given block with previous and/or
 *        next block in heap
 *
 * First checks if previous or next block on "implicit list" on the heap
 * can be coalesced with the current block (is free and not prologue
 * or epilogue).
 *
 * @param[in] block
 * @return The header of the coalesced block
 */
static block_t *coalesce_block(block_t *block) {
    block_t *previous;
    block_t *next = find_next(block);
    if (!get_prev_alloc(block)) {
        previous = find_prev(block);
    }
    /** @brief True if previous block in heap is free and not prologue*/
    bool prev_free = !get_prev_alloc(block);
    /** @brief True if next block in heap is free and not epilogue*/
    bool next_free;
    if ((next != NULL) && (!get_alloc(next))) {
        next_free = true;
    } else {
        next_free = false;
    }

    size_t block_size = get_size(block);

    // Both previous block and next block in heap are free
    if (prev_free && next_free) {
        block = coalesce_prev_next(block, previous, next, block_size);
    }
    // Only previous block is free
    else if (prev_free && (!next_free)) {
        block = coalesce_prev(block, previous, block_size);
    }
    // Only next block is free
    else if ((!prev_free) && next_free) {
        block = coalesce_next(block, next, block_size);
    }
    // Neither previous or next block is free
    else {
        bool prev_alloc = get_prev_alloc(block);
        write_block(block, block_size, false, prev_alloc, get_mini(block));
    }

    return block;
}

/**
 * @brief Alloc extra memory of size size to heap
 *
 * @param[in] size
 * @return Pointer to the header of the allocated block
 */
static block_t *extend_heap(size_t size) {
    void *bp;

    // Allocate an even number of words to maintain alignment
    size = round_up(size, dsize);
    if ((bp = mem_sbrk(size)) == (void *)-1) {
        return NULL;
    }

    // Initialize free block header/footer
    block_t *block = payload_to_header(bp);
    bool prev_alloc = get_prev_alloc(block);
    extend_write(block, size, false, prev_alloc, false);

    // Create new epilogue header
    block_t *block_next = find_next(block);
    write_epilogue(block_next);

    // Coalesce in case the previous block was free
    block = coalesce_block(block);

    insert(block);
    dbg_assert(mm_checkheap(__LINE__));

    return block;
}

/**
 * @brief Splits a block to two smaller blocks that both have
 * a size larger than minimum block size.
 *
 * Splits a block that is larger than the requested size.
 * If the remaining block has a size not smaller than
 * min_block_size, split the block and return the remaining
 * block.
 *
 * @param[in] block
 * @param[in] asize
 * @return A pointer to the unallocated block
 */
static block_t *split_block(block_t *block, size_t asize) {
    dbg_requires(get_alloc(block));
    dbg_requires(asize >= min_block_size);

    size_t block_size = get_size(block);

    if ((block_size - asize) >= min_block_size) {
        bool prev_alloc = get_prev_alloc(block);
        block_t *block_next;
        write_block(block, asize, true, prev_alloc, asize == min_block_size);

        block_next = find_next(block);
        write_block(block_next, block_size - asize, false, true,
                    (block_size - asize) == min_block_size);
        return block_next;
    }

    dbg_ensures(get_alloc(block));

    return NULL;
}

/**
 * @brief Find a block of the requested size and return it to caller.
 *
 * If no satisfactory block can be found, return NULL.
 *
 * @param[in] asize
 * @return The pointer to the allocated block.
 */
static block_t *find_fit(size_t asize) {
    dbg_requires(asize >= dsize);
    block_t *block;

    // Search for a mini block
    if (asize == min_block_size) {
        if (seglist[0] != NULL) {
            return seglist[0];
        }
    }

    size_t bucket = find_bucket(asize);

    while (bucket < BUCKET_NUM) {
        // Search for the smallest block in the first 5 satisfying blocks
        block_t *best = NULL;
        size_t level = 5;
        for (block = seglist[bucket]; block != NULL; block = block->next) {
            if (asize <= get_size(block)) {
                dbg_assert(!get_alloc(block));
                if (best == NULL || get_size(best) > get_size(block)) {
                    best = block;
                }
                level--;
            }
            if (level == 0) {
                break;
            }
        }
        if (best == NULL) {
            bucket++;
        } else {
            return best;
        }
    }

    return NULL; // no fit found
}

/**
 * @brief Checks everything about the heap, the seglist, and blocks
 *
 * As described in the writeup.
 * If and only if there is an error, prints information about the error
 * and return false.
 *
 * @param[in] line
 * @return True if the heap passes the checks, false otherwise.
 */
bool mm_checkheap(int line) {

    block_t *prologue = (block_t *)((word_t *)heap_start - 1);
    block_t *epilogue = (block_t *)((char *)mem_heap_hi() - 7);

    // Checks prologue for size and alloc bit
    if ((size_t)prologue < (size_t)mem_heap_lo() ||
        get_alloc(prologue) == false || get_size(prologue) != 0) {
        dbg_printf("prologue error line %d\n", line);
        return false;
    }

    // Checks epilogue for size and alloc bit
    if (get_alloc(epilogue) == false || get_size(epilogue) != 0) {
        dbg_printf("epilogue error line %d\n", line);
        return false;
    }
    if (get_mini(epilogue)) {
        dbg_printf("epilogue is mini line %d\n", line);
    }

    block_t *block = heap_start;
    block_t *next_in_heap;
    size_t free_count_heap = 0;
    size_t free_count_list = 0;
    bool curr_alloc;

    while (block != epilogue) {
        block_t *footer;
        if (!get_alloc(block) && !get_mini(block)) {
            footer = (block_t *)header_to_footer(block);
        }
        block_t *prev;
        if (!get_prev_alloc(block)) {
            prev = find_prev(block);
        }

        block_t *next = find_next(block);

        // Checks alignment
        if ((!get_mini(block)) && (get_size(block) % dsize != 0)) {
            dbg_printf("payload not aligned line %d\n", line);
            return false;
        }

        // Checks boundaries
        if ((size_t)block < (size_t)heap_start ||
            (size_t)block >
                (size_t)((char *)mem_heap_hi() - 7 - min_block_size)) {
            dbg_printf("Block out of boundary line %d\n", line);
            return false;
        }

        // Checks header and footer consistency
        if (!get_alloc(block) && !get_mini(block)) {
            if ((get_alloc(block) != get_alloc(footer)) ||
                get_size(block) != get_size(footer)) {
                dbg_printf("header and footer inconsistent line %d\n", line);
                return false;
            }
        }

        // Checks coalescing
        if (!get_alloc(block)) {
            // Counts free blocks by iterating through heap
            free_count_heap++;
            // Checks if previous block in heap is also free
            if (!get_prev_alloc(block)) {
                dbg_printf("Consecutive prev free blocks line %d\n", line);
                return false;
            }
            // Checks if next block in heap is also free
            if (next != NULL && next != epilogue && (!get_alloc(next))) {
                dbg_printf("Consecutive free blocks line %d\n", line);
                size_t next_size;
                if (get_mini(next)) {
                    next_size = dsize;
                } else {
                    next_size = get_size(next);
                }
                dbg_printf("nextsize %zu, blocksize %zu\n", next_size,
                           get_size(block));
                return false;
            }
        }

        // Checks pointer consistency
        if (!get_alloc(block)) {
            if (!get_mini(block)) {
                // Checks if previous block points back
                if (block->prev != NULL && block->prev->next != block) {
                    dbg_printf("prev block doesn't point back line %d\n", line);
                    return false;
                }
                // Checks if next block points back
                if (block->next != NULL && block->next->prev != block) {
                    dbg_printf("next block doesn't point back line %d\n", line);
                    return false;
                }
            }
            // Miniblock
            else {
                if (mini_get_next(block) != 0) {
                    block_t *prev_in_list = (block_t *)mini_get_next(block);
                    if (mini_get_header(prev_in_list) != (word_t)block) {
                        dbg_printf("prev-next link fails line %d\n", line);
                        dbg_printf("block addr %zu, prev addr %zu, ",
                                   (word_t)block, (word_t)(prev_in_list));
                        dbg_printf("prev header addr %zu\n",
                                   mini_get_header(prev_in_list));
                        return false;
                    }
                }
                if (mini_get_header(block) != 0) {
                    block_t *next_in_list = (block_t *)mini_get_header(block);
                    if (mini_get_next(next_in_list) != (word_t)block) {
                        dbg_printf("block header %zu", mini_get_header(block));
                        dbg_printf("next-prev link fails line %d\n", line);
                        dbg_printf("mini get next %zu, block %zu",
                                   mini_get_next(next_in_list), (word_t)block);
                        return false;
                    }
                }
            }
        }

        /** Checks consistency between alloc bit of current block and prev_alloc
            bit of next block in heap*/
        curr_alloc = get_alloc(block);
        next_in_heap = find_next(block);
        if (get_prev_alloc(next_in_heap) != curr_alloc) {
            dbg_printf("Alloc bit doesn't match next prev_alloc line %d\n",
                       line);
            return false;
        }

        block = next_in_heap;
    }

    // Traverse each bucket in seglist
    for (size_t bucket = 1; bucket < BUCKET_NUM; bucket++) {
        block_t *tmp = seglist[bucket];
        while (tmp != NULL) {
            free_count_list++;
            if (find_bucket(get_size(tmp)) != bucket) {
                dbg_printf("Block in wrong bucket line %d\n", line);
                dbg_printf("Should in %zu size %zu\n",
                           find_bucket(get_size(tmp)), get_size(tmp));
                return false;
            }
            tmp = tmp->next;
        }
    }
    // Traverse bucket 0 in seglist
    block_t *tmp = seglist[0];
    while (tmp != 0) {
        free_count_list++;
        tmp = (block_t *)mini_get_header(tmp);
    }

    /** Checks if number of free blocks is consistent when counting
        through the heap and iterating through the seglist*/
    if (free_count_heap != free_count_list) {
        dbg_printf("Free counts don't match line %d\n", line);
        dbg_printf("heap free is %zu, list free is %zu\n", free_count_heap,
                   free_count_list);
        return false;
    }

    return true;
}

/**
 * @brief Initializes the heap.
 *
 * Called every time a new heap is required.
 * Initializes all data structures: creates the prologue and the
 * epilogue, extends heap to a initial size of chunksize, sets the
 * block header to the block after the prologue.
 *
 * @return True if the initialization is successful, false otherwise.
 */
bool mm_init(void) {
    // Create the initial empty heap
    word_t *start = (word_t *)(mem_sbrk(2 * wsize));

    if (start == (void *)-1) {
        return false;
    }

    start[0] = pack(0, true, false); // Heap prologue (block footer)
    start[1] = pack(0, true, true);  // Heap epilogue (block header)

    // Heap starts with first "block header", currently the epilogue
    heap_start = (block_t *)&(start[1]);
    for (size_t i = 0; i < BUCKET_NUM; i++) {
        seglist[i] = NULL;
    }

    // Extend the empty heap with a free block of chunksize bytes
    if (extend_heap(chunksize) == NULL) {
        return false;
    }

    dbg_ensures(mm_checkheap(__LINE__));

    return true;
}

/**
 * @brief Allocates a block of requested size from the segregated list.
 *
 * If there is no satisfactory block in list, requests more space from
 * memory. Writes the block as allocated, and splits block if it is large.
 * The returned block should be 16-byte aligned.
 *
 * @param[in] size
 * @return A generic pointer to an allocated block payload of at least
 * size bytes.
 */
void *malloc(size_t size) {
    dbg_requires(mm_checkheap(__LINE__));

    size_t asize;      // Adjusted block size
    size_t extendsize; // Amount to extend heap if no fit is found
    block_t *block;
    void *bp = NULL;

    // Initialize heap if it isn't initialized
    if (heap_start == NULL) {
        mm_init();
    }

    // Ignore spurious request
    if (size == 0) {
        dbg_ensures(mm_checkheap(__LINE__));
        return bp;
    }

    // Adjust block size to include overhead and to meet alignment requirements
    asize = round_up(size + wsize, dsize);
    if (asize < min_block_size) {
        asize = min_block_size;
    }

    // Search the free list for a fit
    block = find_fit(asize);

    // If no fit is found, request more memory, and then and place the block
    if (block == NULL) {
        // Always request at least chunksize
        extendsize = max(asize, chunksize);
        block = extend_heap(extendsize);
        // extend_heap returns an error
        if (block == NULL) {
            return bp;
        }
    }

    // The block should be marked as free
    dbg_assert(!get_alloc(block));

    // Mark block as allocated
    size_t block_size = get_size(block);
    bool prev_alloc = get_prev_alloc(block);
    write_block(block, block_size, true, prev_alloc, get_mini(block));

    delete (block);

    // Try to split the block if too large
    if (block_size > min_block_size) {
        block_t *rest = split_block(block, asize);

        if (rest != NULL) {
            insert(rest);
        }
    }
    bp = header_to_payload(block);

    dbg_ensures(mm_checkheap(__LINE__));
    return bp;
}

/**
 * @brief Frees the block pointed to.
 *
 * free(NULL) has no effect.
 *
 * @pre The pointer passed in must be allocated by previous
 * malloc, calloc or realloc and has not been freed.
 *
 * @param[in] bp
 */
void free(void *bp) {
    dbg_requires(mm_checkheap(__LINE__));

    if (bp == NULL) {
        return;
    }

    block_t *block = payload_to_header(bp);
    size_t size = get_size(block);

    // The block should be marked as allocated
    dbg_assert(get_alloc(block));

    // Mark the block as free
    bool prev_alloc = get_prev_alloc(block);
    write_block(block, size, false, prev_alloc, get_mini(block));

    // Try to coalesce the block with its neighbors
    block = coalesce_block(block);
    insert(block);

    dbg_ensures(mm_checkheap(__LINE__));
}

/**
 * @brief Allocates a block of at least size bytes.
 *
 * If ptr is NULL, calls malloc(size)
 * If size is 0, calls free(ptr) and returns NULL
 * Else calls free(ptr) followed by malloc(size), where the
 * contents of the new block will be the same as those of the
 * old block, up to the minimum of the old and new sizes.
 *
 * @param[in] ptr
 * @param[in] size
 * @return A generic pointer to an allocated block payload.
 */
void *realloc(void *ptr, size_t size) {
    block_t *block = payload_to_header(ptr);
    size_t copysize;
    void *newptr;

    // If size == 0, then free block and return NULL
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    // If ptr is NULL, then equivalent to malloc
    if (ptr == NULL) {
        return malloc(size);
    }

    // Otherwise, proceed with reallocation
    newptr = malloc(size);

    // If malloc fails, the original block is left untouched
    if (newptr == NULL) {
        return NULL;
    }

    // Copy the old data
    copysize = get_payload_size(block); // gets size of old payload
    if (size < copysize) {
        copysize = size;
    }
    memcpy(newptr, ptr, copysize);

    // Free the old block
    free(ptr);

    return newptr;
}

/**
 * @brief Allocates memory for an array of elements elements of
 * size bytes each.
 *
 * The memory is set to zero before returning.
 *
 * @param[in] elements
 * @param[in] size
 * @return A generic pointer to an allocated block payload.
 */
void *calloc(size_t elements, size_t size) {
    void *bp;
    size_t asize = elements * size;

    if (elements == 0) {
        return NULL;
    }
    if (asize / elements != size) {
        // Multiplication overflowed
        return NULL;
    }

    bp = malloc(asize);
    if (bp == NULL) {
        return NULL;
    }

    // Initialize all bits to 0
    memset(bp, 0, asize);

    return bp;
}

/*
 *****************************************************************************
 * Do not delete the following super-secret(tm) lines!                       *
 *                                                                           *
 * 53 6f 20 79 6f 75 27 72 65 20 74 72 79 69 6e 67 20 74 6f 20               *
 *                                                                           *
 * 66 69 67 75 72 65 20 6f 75 74 20 77 68 61 74 20 74 68 65 20               *
 * 68 65 78 61 64 65 63 69 6d 61 6c 20 64 69 67 69 74 73 20 64               *
 * 6f 2e 2e 2e 20 68 61 68 61 68 61 21 20 41 53 43 49 49 20 69               *
 *                                                                           *
 * 73 6e 27 74 20 74 68 65 20 72 69 67 68 74 20 65 6e 63 6f 64               *
 * 69 6e 67 21 20 4e 69 63 65 20 74 72 79 2c 20 74 68 6f 75 67               *
 * 68 21 20 2d 44 72 2e 20 45 76 69 6c 0a c5 7c fc 80 6e 57 0a               *
 *                                                                           *
 *****************************************************************************
 */