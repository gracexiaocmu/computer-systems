/*
 * Cache file for proxy lab.
 *
 * Contains functions that initiate a cache, retrieve a request from
 * cache, and insert a request into cache respectively.
 *
 *
 * Author: Yuxuan Xiao (yuxuanx)
 */

#include "cache.h"

#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>

#include <errno.h>
#include <http_parser.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>

/*
 * Max cache and object sizes
 *
 */
#define MAX_CACHE_SIZE (1024 * 1024)
#define MAX_OBJECT_SIZE (100 * 1024)

/**
 * Create a new queue that represents a cache
 */
queue_t *queue_new(void) {
    queue_t *q = malloc(sizeof(queue_t));
    if (q == NULL) {
        return NULL;
    }
    q->head = NULL;
    q->tail = NULL;
    q->byte = 0;
    return q;
}

/**
 * Free the queue (ie the cache)
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

/*
 * retrieve a request and its corresponding response from the cache
 * based on a given uri, update the cache based on lru accordingly.
 */
list_ele_t *uri_get_response(queue_t *cache, const char *uri) {
    list_ele_t *temp = cache->head;
    list_ele_t *prev = NULL;

    // Loop through the cache to search for the uri
    while (temp != NULL) {
        if (strncmp(temp->uri, uri, MAX_OBJECT_SIZE) == 0) {
            // Update the cache, put the requested node at the tail
            if (prev == NULL) {
                cache->tail->next = temp;
                cache->tail = temp;
                cache->head = temp->next;
                temp->next = NULL;
            } else {
                cache->tail->next = temp;
                cache->tail = temp;
                prev->next = temp->next;
                temp->next = NULL;
            }
            return temp;
        }
        prev = temp;
        temp = temp->next;
    }
    // Request not found
    return NULL;
}

/*
 * Insert a uri and its corresponding response into the cache,
 * evict the least-recently-used request from the cache when the cache is
 * full.
 */
void cache_insert(queue_t *cache, const char *uri, const char *resp, int size) {
    if (uri_get_response(cache, uri) != NULL) {
        return;
    }
    int max_byte = MAX_CACHE_SIZE - size;
    list_ele_t *tmp;
    // Evicts lru requests to make enough space for the new request
    while (cache->byte > max_byte) {
        tmp = cache->head;
        cache->head = tmp->next;
        if (cache->head == NULL) {
            cache->tail = NULL;
            break;
        }
        cache->byte -= tmp->size;
        (tmp->count)--;
        if (tmp->count == 0) {
            free((void *)tmp->uri);
            free((void *)tmp->response);
            free(tmp);
        }
    }

    // Create a new node for the request
    list_ele_t *new = malloc(sizeof(list_ele_t));
    new->size = size;
    new->next = NULL;
    new->count = 1;
    new->uri = malloc(strlen(uri) + 1);
    strcpy(new->uri, uri);
    new->response = malloc(size);
    memcpy(new->response, resp, size);

    // Insert the new node at the tail of the cache
    if (cache->head == NULL) {
        cache->head = new;
        cache->tail = new;
    } else {
        cache->tail->next = new;
        cache->tail = new;
    }
    cache->byte += size;
    return;
}