/*
 * Cache header file for proxy lab.
 *
 *
 *
 *
 * Author: Yuxuan Xiao (yuxuanx)
 */

#ifndef CACHE_H
#define CACHE_H

#include <stdbool.h>
#include <stdlib.h>

/**
 * Linked list element representing a key-value pair
 */
typedef struct list_ele {
    char *uri;
    char *response;
    int size;
    int count;
    struct list_ele *next;
} list_ele_t;

/**
 * A linked list that represents a cache
 */
typedef struct {
    list_ele_t *head;
    list_ele_t *tail;
    int byte;
} queue_t;

queue_t *queue_new(void);
void queue_free(queue_t *q);
list_ele_t *uri_get_response(queue_t *cache, const char *uri);
void cache_insert(queue_t *cache, const char *uri, const char *resp, int size);

#endif /* CACHE_H */