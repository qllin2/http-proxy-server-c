#ifndef PROXY_H
#define PROXY_H
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <stdint.h>

#define BACKLOG 10
#define MAX_REQUEST_HEADERS_SIZE 8192 // Max size for all request headers from client
#define MAX_RESPONSE_HEADERS_SIZE 8192 // Max size for all response headers from origin server
#define BUFFER_SIZE 8192          // For reading/writing data in chunks
#define MAX_REQUEST_LINE_SIZE 8192
#define MAX_METHOD_SIZE 16
#define MAX_URI_SIZE 1024
#define MAX_VERSION_SIZE 16
#define MAX_CACHE_ENTRIES 10
#define MAX_CACHE_KEY_SIZE 2000
#define MAX_CACHE_VALUE_SIZE (100 * 1024) // 100KB

typedef struct CacheEntry {
    char *request_key;
    char *response_data;
    size_t response_data_len;

    char *host_for_log;
    char *uri_for_log;
    
    time_t expires_at;  // Expiration timestamp (0 means no expiration)

    struct CacheEntry *prev;
    struct CacheEntry *next;
} CacheEntry;

typedef struct LRUCache {
    CacheEntry *head;
    CacheEntry *tail;
    int count;
    int capacity;
} LRUCache;

void print_log(const char *format, ...);
void log_accepted();
void log_request_tail(const char *last_line);
void log_getting(const char *host, const char *request_uri);
void log_response_body_length(long length);
void log_serving_from_cache(const char *host, const char *request_uri);
void log_evicting_from_cache(const char *host, const char *request_uri);
void log_not_caching(const char *host, const char *request_uri);
void log_stale_entry(const char *host, const char *request_uri);
void parse_request_line(const char *request_line, char *method, char *uri, char *version);
char *get_header_value(const char *headers, const char *name);
bool is_cacheable_response(const char *response_headers);
uint32_t parse_max_age(const char *cache_control_header);
void handle_request(int conn_fd, LRUCache *cache, bool cache_enabled);

// Cache functions
void cache_init(LRUCache *cache, int capacity);
void cache_detach_node(LRUCache *cache, CacheEntry *entry);
void cache_attach_to_head(LRUCache *cache, CacheEntry *entry);
CacheEntry *cache_get(LRUCache *cache, const char *request_key);
void cache_evict(LRUCache *cache, CacheEntry *entry_to_evict);
void cache_evict_lru(LRUCache *cache);
void cache_cleanup(LRUCache *cache);
void cache_put(LRUCache *cache, const char *request_key, size_t request_key_len, 
               const char *response_data, size_t response_data_len, 
               const char *host, const char *uri, long response_body_actual_len,
               const char *response_headers);
void cache_replace_entry(LRUCache *cache, CacheEntry *cached_entry, const char *request_key, size_t request_key_len,
               const char *response_data, size_t response_data_len, 
               const char *host, const char *uri, long response_body_actual_len,
               const char *response_headers);

#endif // PROXY_H
