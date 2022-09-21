/*
 * Starter code for proxy lab.
 * Checkpoint: a sequential web proxy. It listens on a specific port
 * and accept connection from clients. It then parses the request from
 * the client and forwards it to the server. When it gets response from
 * the server, it returns the response to the client.
 *
 * Final: A concurrent web proxy with cache. It spawns threads to deal
 * with multiple requests at the same time. It also stores request-response
 * pairs in a cache based on lru.
 *
 * Author: Yuxuan Xiao (yuxuanx)
 */

/* Some useful includes to help you get started */

#include "cache.h"
#include "csapp.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <errno.h>
#include <http_parser.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>

/*
 * Debug macros, which can be enabled by adding -DDEBUG in the Makefile
 * Use these if you find them useful, or delete them if not
 */
#ifdef DEBUG
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_printf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dbg_assert(...)
#define dbg_printf(...)
#endif

/*
 * Max cache and object sizes
 * You might want to move these to the file containing your cache implementation
 */
#define MAX_CACHE_SIZE (1024 * 1024)
#define MAX_OBJECT_SIZE (100 * 1024)

/*
 * String to use for the User-Agent header.
 * Don't forget to terminate with \r\n
 */
static const char *header_user_agent = "Mozilla/5.0"
                                       " (X11; Linux x86_64; rv:3.10.0)"
                                       " Gecko/20191101 Firefox/63.0.1";

pthread_mutex_t mutex;
queue_t *c;

/**
 * @brief Serves the client
 * It parses the request from the client and sends it to the server.
 * Then it gets the response from the server and sends if back to
 * the client.
 *
 * @param[in] connectfd
 */
void serve(int connectfd) {
    rio_t rp;
    rio_readinitb(&rp, connectfd);
    parser_t *parsed = parser_new();
    char usrbuf[MAXLINE];
    rio_readlineb(&rp, (void *)usrbuf, MAXLINE);

    // Parses every line in the request
    while (strncmp(usrbuf, "\r\n", 2) != 0) {
        parser_state state;
        if ((state = parser_parse_line(parsed, (const char *)usrbuf)) ==
            ERROR) {
            fprintf(stderr, "parse line error\n");
            parser_free(parsed);
            return;
        }
        rio_readlineb(&rp, usrbuf, MAXLINE);
    }

    // Retrieves every part in the parsed file
    const char *method;
    const char *host;
    const char *port;
    const char *path;
    const char *uri;
    parser_retrieve(parsed, METHOD, &method);
    parser_retrieve(parsed, HOST, &host);
    parser_retrieve(parsed, PORT, &port);
    parser_retrieve(parsed, PATH, &path);
    parser_retrieve(parsed, URI, &uri);

    // Makes sure the method is GET
    if (strncmp(method, "GET", 3) != 0) {
        rio_writen(connectfd,
                   "HTTP/1.0 501 Not Implemented\r\nContent-Length: 0\r\n\r\n",
                   PARSER_MAXLINE);
        parser_free(parsed);
        return;
    }

    // If uri is in cache, writes the response in cache to client
    list_ele_t *resp_node;
    pthread_mutex_lock(&mutex);
    resp_node = uri_get_response(c, uri);
    pthread_mutex_unlock(&mutex);

    // uri is found in cache
    if (resp_node != NULL) {
        pthread_mutex_lock(&mutex);
        const char *ctnt = resp_node->response;
        int s = resp_node->size;
        (resp_node->count)++;
        pthread_mutex_unlock(&mutex);
        rio_writen(connectfd, ctnt, s);
        pthread_mutex_lock(&mutex);
        (resp_node->count)--;
        if (resp_node->count == 0) {
            free((void *)resp_node->uri);
            free((void *)resp_node->response);
            free(resp_node);
        }
        pthread_mutex_unlock(&mutex);
        parser_free(parsed);
        return;
    }

    // Sends connection request to the server
    int clientfd;
    if ((clientfd = open_clientfd(host, port)) < 0) {
        fprintf(stderr, "open clientfd error\n");
        parser_free(parsed);
        return;
    }

    // Stores the headers in req
    char req[MAXLINE];
    snprintf(req, MAXLINE,
             "%s %s HTTP/1.0\r\n"
             "Host: %s:%s\r\n"
             "User-Agent: %s\r\n"
             "Connection: close\r\n"
             "Proxy-Connection: close\r\n",
             method, path, host, port, header_user_agent);

    // Extra headers
    header_t *header;
    while ((header = parser_retrieve_next_header(parsed)) != NULL) {
        if (strncmp(header->name, "Host", 4) != 0 &&
            strncmp(header->name, "User-Agent", 10) != 0 &&
            strncmp(header->name, "Connection", 10) != 0 &&
            strncmp(header->name, "Proxy-Connection", 16) != 0) {
            strncat(req, header->name, strlen(header->name));
            strncat(req, ": ", 2);
            strncat(req, header->value, strlen(header->value));
            strncat(req, "\r\n", 2);
        }
    }
    strncat(req, "\r\n", 2);
    // Writes to the server
    if (rio_writen(clientfd, req, sizeof(req)) == -1) {
        fprintf(stderr, "rio_writen error\n");
        parser_free(parsed);
        return;
    }

    // Reads from server and writes the response back to the client
    size_t total = 0;
    size_t n;
    rio_t clt;
    rio_readinitb(&clt, clientfd);
    char content[MAX_OBJECT_SIZE] = {""};
    while ((n = rio_readnb(&clt, (void *)usrbuf, MAXLINE)) > 0) {
        rio_writen(connectfd, usrbuf, n);
        // Adds the response line to the local variable
        if (total + n <= MAX_OBJECT_SIZE) {
            memcpy(content + total, usrbuf, n);
        }
        total += n;
        memset(usrbuf, 0, MAXLINE);
    }
    if (n < 0) {
        fprintf(stderr, "Response error\n");
        parser_free(parsed);
        return;
    }
    // Inserts the request and response pair into the cache
    if (total <= MAX_OBJECT_SIZE) {
        pthread_mutex_lock(&mutex);
        cache_insert(c, uri, (const char *)(content), total);
        pthread_mutex_unlock(&mutex);
    }
    parser_free(parsed);
    return;
}

/**
 * @brief Thread routine
 * Calls serve to deal with the request, and frees the vargp when finished
 *
 * @param[in] vargp
 */
void *thread(void *vargp) {
    pthread_detach(pthread_self());
    int connectfd = *((int *)vargp);
    free(vargp);
    serve(connectfd);
    close(connectfd);
    return NULL;
}

/**
 * @brief Main function that takes in commmand line and functions
 * as a server.
 *
 * @param[in] argc
 * @param[in] argv
 * @return an integer
 */
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    printf("%s", header_user_agent);
    pthread_t tid;
    const char *my_port = argv[1];
    pthread_mutex_init(&mutex, NULL);
    c = queue_new();

    // Listens to the port given in command line
    int listenfd;
    if ((listenfd = open_listenfd(my_port)) < 0) {
        fprintf(stderr, "open listenfd error\n");
    }

    // Listens to the port until there is a request from the client
    while (1) {
        struct sockaddr_in socket_addr;
        socklen_t addrlen = sizeof(socket_addr);
        int *connectfd = malloc(sizeof(int));
        *connectfd =
            accept(listenfd, (struct sockaddr *)&socket_addr, &addrlen);
        if (*connectfd < 0) {
            sio_printf("accept error\n");
            continue;
        }
        // Spawns a new thread to deal with the request
        pthread_create(&tid, NULL, thread, connectfd);
    }

    return 0;
}
