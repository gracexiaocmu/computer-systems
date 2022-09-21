/**
 * Name: Yuxuan (Grace) Xiao
 * AndrewID: yuxuanx
 */
#include "cachelab.h"
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINE_LENGTH 80

/**
 * Global variables for number of dirty bytes in cache and
 * number of dirty bytes evicted, can be used and modified
 * by all functions
 */
unsigned int dirty_in_cache = 0;
unsigned int dirty_evicted = 0;

/**
 * Linked list element representing a line in cache
 */
typedef struct list_ele {
    int dirty;
    long tag;
    struct list_ele *next;
} list_ele_t;

/**
 * A linked list that represents a set in cache
 */
typedef struct {
    list_ele_t *head;
    list_ele_t *tail;
    int size;
} queue_t;

/**
 * Create a new queue that represents a set
 */
queue_t *queue_new(void) {
    queue_t *q = malloc(sizeof(queue_t));
    if (q == NULL) {
        return NULL;
    }
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
    return q;
}

/**
 * Free the queue (ie a set in cache)
 */
void queue_free(queue_t *q) {
    if (q != NULL) {
        if (q->head != NULL) {
            list_ele_t *temp = q->head;
            while (temp != NULL) {
                temp = temp->next;
                free(q->head);
                q->head = temp;
            }
        }
        free(q);
    }
}

/**
 * Free the whole cache
 */
void cache_free(queue_t **c, int s) {
    if (c != NULL) {
        for (int i = 0; i < s; i++) {
            queue_free(c[i]);
        }
        free(c);
    }
}

/**
 * Simulate the operation in cache of one line in the trace file
 * Return hit('h'), miss('m'), or miss eviction('e')
 * Change global variables to keep track of dirty bits
 */
char cache_insert(queue_t *qset, long address, int entry, int set, int block,
                  char *op) {
    long tag = address >> (set + block);

    // Check if there's a hit
    list_ele_t *temp = qset->head;
    list_ele_t *prev = NULL;
    while (temp != NULL) {
        if (temp->tag == tag) { // hit
            if (strcmp(op, "S") == 0) {
                if (temp->dirty == 0) {
                    dirty_in_cache++;
                }
                temp->dirty = 1;
            }
            if ((temp != qset->tail) && (temp == qset->head)) {
                qset->head = temp->next;
                qset->tail->next = temp;
                qset->tail = temp;
                temp->next = NULL;
            } else if (temp != qset->tail) {
                prev->next = temp->next;
                qset->tail->next = temp;
                qset->tail = temp;
                temp->next = NULL;
            }
            return 'h';
        }
        prev = temp;
        temp = temp->next;
    }

    // It's a miss, and we need to decide if there's an eviction
    if (qset->size == entry) { // eviction
        temp = qset->head;
        qset->tail->next = temp;
        if (temp->next != NULL) {
            qset->head = temp->next;
        }
        qset->tail = temp;
        temp->next = NULL;
        temp->tag = tag;
        if (strcmp(op, "S") == 0) {
            if (temp->dirty == 1) {
                dirty_evicted++;
            } else {
                dirty_in_cache++;
            }
            temp->dirty = 1;
        } else { // load
            if (temp->dirty == 1) {
                dirty_evicted++;
                dirty_in_cache--;
            }
            temp->dirty = 0;
        }
        return 'e';
    } else { // Miss but no eviction, create a new list node
        list_ele_t *newt;
        if (qset == NULL) {
            return 'f';
        }
        newt = malloc(sizeof(list_ele_t));
        if (newt == NULL) {
            return 'f';
        }
        newt->tag = tag;
        newt->next = NULL;
        newt->dirty = 0;
        if (qset->head == NULL) {
            qset->head = newt;
        } else {
            qset->tail->next = newt;
        }
        qset->size++;
        qset->tail = newt;
        if (strcmp(op, "S") == 0) {
            newt->dirty = 1;
            dirty_in_cache++;
        }
        return 'm';
    }
    return 'f';
}

/**
 * Main function that reads command line and simulates cache
 * operations with the given trace file.
 * Call printSummary
 */
int main(int argc, char *argv[]) {
    int set, entry, block, opt;
    set = 0;
    entry = 0;
    block = 0;
    char *text = NULL;
    unsigned int hits, misses, evictions;
    hits = 0;
    misses = 0;
    evictions = 0;

    // Read command line flags and arguments
    while ((opt = getopt(argc, argv, "s:E:b:t:")) != -1) {
        switch (opt) {
        case 's':
            set = atoi(optarg);
            printf("set:%d\n", set);
            break;
        case 'E':
            entry = atoi(optarg);
            printf("entry:%d\n", entry);
            break;
        case 'b':
            block = atoi(optarg);
            printf("block:%d\n", block);
            break;
        case 't':
            text = optarg;
            printf("file:%s\n", text);
            break;
        default:
            printf("Wrong flag or missing argument.\n");
            exit(EXIT_FAILURE);
        }
    }

    if (optind < argc) {
        printf("Expected argument after options\n");
        exit(EXIT_FAILURE);
    }

    FILE *fptr;

    fptr = fopen(text, "r");
    if (fptr == NULL) {
        printf("file doesn't exist\n");
        exit(1);
    }
    char *line = malloc(sizeof(char) * MAX_LINE_LENGTH);

    // Assign set to 1 even when it's 0 to initialize cache properly
    unsigned long cache_size = (unsigned long)pow(2, set);
    if (set == 0) {
        cache_size = 1;
    }

    // Initialize cache memory with cache size of 'set' and set size of 'entry'
    queue_t **cache = malloc(sizeof(queue_t) * cache_size);
    for (int i = 0; i < (int)cache_size; i++) {
        cache[i] = queue_new();
    }

    // Iterate through each line and simulate the cache operations
    while (fgets(line, MAX_LINE_LENGTH, fptr)) {
        char *op = strtok(line, " ");
        char *nums = strtok(NULL, " ");
        char *addr = strtok(nums, ",");
        // char *bsize = strtok(NULL, ",");
        long address = (long)strtol(addr, NULL, 16);
        int set_number = (address >> block) & (int)(pow(2, (set)) - 1);
        char result =
            cache_insert(cache[set_number], address, entry, set, block, op);
        switch (result) {
        case 'h':
            hits++;
            break;
        case 'm':
            misses++;
            break;
        case 'e':
            evictions++;
            misses++;
            break;
        default:
            printf("fail to insert cache\n");
            exit(EXIT_FAILURE);
        }
    }

    // Create a csim_stats_t variable and pass in the results
    // Then call printSummary
    csim_stats_t *stat = malloc(sizeof(csim_stats_t));
    if (stat == NULL) {
        printf("null stat struct\n");
        exit(EXIT_FAILURE);
    }
    stat->dirty_bytes = dirty_in_cache * (unsigned int)pow(2, block);
    stat->dirty_evictions = dirty_evicted * (unsigned int)pow(2, block);
    stat->evictions = evictions;
    stat->hits = hits;
    stat->misses = misses;
    printSummary(stat);

    // Free memory and close file
    fclose(fptr);
    free(line);
    cache_free(cache, (int)cache_size);
    free(stat);

    exit(EXIT_SUCCESS);
}
