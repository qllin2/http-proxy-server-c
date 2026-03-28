#include "proxy.h"

typedef struct WorkQueue {
    int fds[BACKLOG * 32];
    int head;
    int tail;
    int count;
    bool shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} WorkQueue;

typedef struct WorkerArgs {
    WorkQueue *queue;
    LRUCache *cache;
    bool cache_enabled;
} WorkerArgs;

#define THREAD_POOL_SIZE 8

static void queue_init(WorkQueue *queue) {
    memset(queue, 0, sizeof(*queue));
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    pthread_cond_init(&queue->not_full, NULL);
}

static void queue_destroy(WorkQueue *queue) {
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
}

static bool queue_push(WorkQueue *queue, int fd) {
    pthread_mutex_lock(&queue->mutex);
    while (!queue->shutting_down && queue->count >= (int)(sizeof(queue->fds) / sizeof(queue->fds[0]))) {
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    }
    if (queue->shutting_down) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    queue->fds[queue->tail] = fd;
    queue->tail = (queue->tail + 1) % (int)(sizeof(queue->fds) / sizeof(queue->fds[0]));
    queue->count++;
    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

static bool queue_pop(WorkQueue *queue, int *fd_out) {
    pthread_mutex_lock(&queue->mutex);
    while (!queue->shutting_down && queue->count == 0) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }
    if (queue->shutting_down && queue->count == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    *fd_out = queue->fds[queue->head];
    queue->head = (queue->head + 1) % (int)(sizeof(queue->fds) / sizeof(queue->fds[0]));
    queue->count--;
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

static void *worker_main(void *arg) {
    WorkerArgs *ctx = (WorkerArgs *)arg;
    int conn_fd = -1;
    while (queue_pop(ctx->queue, &conn_fd)) {
        handle_request(conn_fd, ctx->cache, ctx->cache_enabled);
        close(conn_fd);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int listen_fd, conn_fd;
    struct sockaddr_in6 server_addr; // IPv6 allows for IPv4 too
    struct sockaddr_storage client_addr;
    bool cache_enabled = false;
    socklen_t sin_size;
    int port = 8080;

    // Prevent process termination when writing to a closed socket.
    signal(SIGPIPE, SIG_IGN);

    // Initialize cache
    LRUCache cache;
    cache_init(&cache, MAX_CACHE_ENTRIES);

    // parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-c") == 0) {
            cache_enabled = true;
        }
    }

    // create socket
    if ((listen_fd = socket(AF_INET6, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // set SO_REUSEADDR
    int enable = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) == -1) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // bind socket to address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_port = htons(port);
    server_addr.sin6_addr = in6addr_any;

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, BACKLOG) == -1) {
        perror("listen");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    WorkQueue queue;
    queue_init(&queue);

    const int worker_count = THREAD_POOL_SIZE;
    pthread_t workers[THREAD_POOL_SIZE];
    int workers_started = 0;
    WorkerArgs worker_ctx = {
        .queue = &queue,
        .cache = &cache,
        .cache_enabled = cache_enabled
    };
    for (int i = 0; i < worker_count; i++) {
        if (pthread_create(&workers[i], NULL, worker_main, &worker_ctx) != 0) {
            perror("pthread_create");
            pthread_mutex_lock(&queue.mutex);
            queue.shutting_down = true;
            pthread_cond_broadcast(&queue.not_empty);
            pthread_cond_broadcast(&queue.not_full);
            pthread_mutex_unlock(&queue.mutex);
            for (int j = 0; j < workers_started; j++) {
                pthread_join(workers[j], NULL);
            }
            close(listen_fd);
            queue_destroy(&queue);
            cache_cleanup(&cache);
            exit(EXIT_FAILURE);
        }
        workers_started++;
    }

    while (1) {
        sin_size = sizeof(client_addr);
        if ((conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &sin_size)) == -1) {
            perror("accept");
            continue;
        }

        log_accepted();
        if (!queue_push(&queue, conn_fd)) {
            close(conn_fd);
            break;
        }
    }

    pthread_mutex_lock(&queue.mutex);
    queue.shutting_down = true;
    pthread_cond_broadcast(&queue.not_empty);
    pthread_cond_broadcast(&queue.not_full);
    pthread_mutex_unlock(&queue.mutex);

    for (int i = 0; i < worker_count; i++) {
        pthread_join(workers[i], NULL);
    }

    // close listening socket
    close(listen_fd);
    queue_destroy(&queue);

    // Clean up cache before exit
    cache_cleanup(&cache);

    return 0;
}

void print_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

void log_accepted() {
    print_log("Accepted");
}

void log_request_tail(const char *last_line) {
    // Omit trailing \r\n from last_line before printing
    // This function expects last_line to already have \r\n removed if necessary
    print_log("Request tail %s", last_line);
}

void log_getting(const char *host, const char *request_uri) {
    // Single space between host and request-URI
    print_log("GETting %s %s", host, request_uri);
}

void log_response_body_length(long length) {
    if (length >= 0) {
        print_log("Response body length %ld", length);
    } else {
        print_log("Response body length not found or invalid");
    }
}

void log_serving_from_cache(const char *host, const char *request_uri) {
    print_log("Serving %s %s from cache", host, request_uri);
}

void log_evicting_from_cache(const char *host, const char *request_uri) {
    print_log("Evicting %s %s from cache", host, request_uri);
}

void log_not_caching(const char *host, const char *request_uri) {
    print_log("Not caching %s %s", host, request_uri);
}

void log_stale_entry(const char *host, const char *request_uri) {
    print_log("Stale entry for %s %s", host, request_uri);
}

// Check if response is cacheable based on Cache-Control header
bool is_cacheable_response(const char *response_headers) {
    if (!response_headers) {
        return false;
    }
    char *cache_control = get_header_value(response_headers, "Cache-Control");
    if (!cache_control) {
        return true; // No Cache-Control header means cacheable
    }
    
    // Check for non-cacheable directives
    bool cacheable = true;
    
    // Check for private
    if (strcasestr(cache_control, "private")) {
        cacheable = false;
    }
    // Check for no-store
    else if (strcasestr(cache_control, "no-store")) {
        cacheable = false;
    }
    // Check for no-cache
    else if (strcasestr(cache_control, "no-cache")) {
        cacheable = false;
    }
    // Check for max-age=0
    else if (strcasestr(cache_control, "max-age=0")) {
        cacheable = false;
    }
    // Check for must-revalidate
    else if (strcasestr(cache_control, "must-revalidate")) {
        cacheable = false;
    }
    // Check for proxy-revalidate
    else if (strcasestr(cache_control, "proxy-revalidate")) {
        cacheable = false;
    }
    
    free(cache_control);
    return cacheable;
}

// Parse max-age value from Cache-Control header
uint32_t parse_max_age(const char *cache_control_header) {
    if (!cache_control_header) {
        return 0; // No header means no max-age
    }
    
    char *max_age_pos = strcasestr(cache_control_header, "max-age=");
    if (!max_age_pos) {
        return 0; // No max-age directive
    }
    
    // Move past "max-age="
    max_age_pos += 8;
    
    // Parse the number
    uint32_t max_age = 0;
    while (*max_age_pos >= '0' && *max_age_pos <= '9') {
        max_age = max_age * 10 + (*max_age_pos - '0');
        max_age_pos++;
    }
    
    return max_age;
}

void parse_request_line(const char *request_line, char *method, char *uri, char *version) {
    method[0] = '\0';
    uri[0] = '\0';
    version[0] = '\0';

    sscanf(request_line, "%s %s %s", method, uri, version);
}

char *get_header_value(const char *headers, const char *name) {
    // Look for the header name at the beginning of a line
    size_t name_len = strlen(name);
    char *pos = (char *)headers;

    while ((pos = strcasestr(pos, name)) != NULL) {
        // Check if this is at the beginning of a line
        if (pos == headers || *(pos - 1) == '\n') {
            // Check if followed by colon
            if (pos[name_len] == ':') {
                // Found the header, extract the value
                char *value_start = pos + name_len + 1; // skip name and ':'
                while (*value_start == ' ') value_start++; // skip spaces

                char *value_end = strstr(value_start, "\r\n");
                if (!value_end) return NULL;

                size_t value_len = value_end - value_start;
                char *value = (char *)malloc(value_len + 1);
                if (!value) {
                    perror("malloc for get_header_value failed");
                    return NULL;
                }
                strncpy(value, value_start, value_len);
                value[value_len] = '\0';
                return value;
            }
        }
        pos++; // Continue searching
    }
    return NULL;
}

// remove square brackets from IPv6 address if present
char *strip_ipv6_brackets(const char *host) {
    char *clean_host = strdup(host);
    if (!clean_host) return NULL;

    size_t len = strlen(clean_host);
    if (len >= 2 && clean_host[0] == '[' && clean_host[len-1] == ']') {
        clean_host[len-1] = '\0';
        memmove(clean_host, clean_host + 1, len - 1);
    }
    return clean_host;
}

static ssize_t send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

static long parse_content_length_header(const char *headers) {
    if (!headers) {
        return -1;
    }
    char *cl_header = strcasestr(headers, "Content-Length:");
    if (!cl_header) {
        return -1;
    }
    cl_header += 15;
    while (*cl_header == ' ') {
        cl_header++;
    }
    return strtol(cl_header, NULL, 10);
}

static bool read_request_headers_with_state_machine(int conn_fd,
                                                    char **out_buffer,
                                                    size_t *out_header_len,
                                                    char **out_prefetched_body,
                                                    size_t *out_prefetched_body_len,
                                                    char *last_header_line,
                                                    size_t last_header_line_cap) {
    char *buffer = NULL;
    size_t capacity = BUFFER_SIZE;
    size_t size = 0;
    size_t header_len = 0;
    bool headers_complete = false;
    size_t scanned_header_bytes = 0;
    int marker_state = 0;
    char recv_chunk[BUFFER_SIZE];

    buffer = (char *)malloc(capacity);
    if (!buffer) {
        perror("malloc request buffer failed");
        return false;
    }

    while (1) {
        ssize_t n = recv(conn_fd, recv_chunk, sizeof(recv_chunk), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv request headers from client");
            free(buffer);
            return false;
        }
        if (n == 0) {
            if (!headers_complete) {
                print_log("Client closed connection before sending full request headers");
                free(buffer);
                return false;
            }
            break;
        }

        size_t needed = size + (size_t)n + 1;
        size_t max_request_read = MAX_REQUEST_HEADERS_SIZE + BUFFER_SIZE + 1;
        if (needed > max_request_read) {
            print_log("Request too large while parsing headers");
            free(buffer);
            return false;
        }
        if (needed > capacity) {
            size_t new_capacity = capacity;
            while (new_capacity < needed) {
                new_capacity *= 2;
            }
            char *new_buffer = realloc(buffer, new_capacity);
            if (!new_buffer) {
                perror("realloc request buffer failed");
                free(buffer);
                return false;
            }
            buffer = new_buffer;
            capacity = new_capacity;
        }

        memcpy(buffer + size, recv_chunk, (size_t)n);

        if (!headers_complete) {
            for (ssize_t i = 0; i < n; i++) {
                char c = recv_chunk[i];
                scanned_header_bytes++;
                if (scanned_header_bytes > MAX_REQUEST_HEADERS_SIZE) {
                    print_log("Request headers too large, max size is %d bytes", MAX_REQUEST_HEADERS_SIZE);
                    free(buffer);
                    return false;
                }

                if (marker_state == 0) {
                    marker_state = (c == '\r') ? 1 : 0;
                } else if (marker_state == 1) {
                    if (c == '\n') {
                        marker_state = 2;
                    } else if (c == '\r') {
                        marker_state = 1;
                    } else {
                        marker_state = 0;
                    }
                } else if (marker_state == 2) {
                    marker_state = (c == '\r') ? 3 : 0;
                } else if (marker_state == 3) {
                    if (c == '\n') {
                        headers_complete = true;
                        header_len = size + (size_t)i + 1;
                        break;
                    } else if (c == '\r') {
                        marker_state = 1;
                    } else {
                        marker_state = 0;
                    }
                }
            }
        }

        size += (size_t)n;
        buffer[size] = '\0';

        if (headers_complete) {
            break;
        }
    }

    if (!headers_complete) {
        free(buffer);
        return false;
    }

    buffer[header_len] = '\0';

    // Extract the last header line (line before CRLFCRLF).
    if (last_header_line && last_header_line_cap > 0) {
        last_header_line[0] = '\0';
        if (header_len >= 4) {
            size_t end = header_len - 4;
            size_t start = end;
            while (start > 0) {
                if (buffer[start - 1] == '\n' && start >= 2 && buffer[start - 2] == '\r') {
                    break;
                }
                start--;
            }
            size_t line_len = end > start ? (end - start) : 0;
            if (line_len > 0) {
                size_t copy_len = line_len < (last_header_line_cap - 1) ? line_len : (last_header_line_cap - 1);
                memcpy(last_header_line, buffer + start, copy_len);
                last_header_line[copy_len] = '\0';
            }
        }
    }

    *out_buffer = buffer;
    *out_header_len = header_len;
    *out_prefetched_body = buffer + header_len;
    *out_prefetched_body_len = size - header_len;
    return true;
}

static bool cache_get_response_copy(LRUCache *cache,
                                    const char *request_key,
                                    char **response_copy,
                                    size_t *response_len,
                                    bool *stale_found) {
    *response_copy = NULL;
    *response_len = 0;
    *stale_found = false;

    pthread_rwlock_wrlock(&cache->lock);
    CacheEntry *entry = cache_get(cache, request_key);
    if (!entry) {
        pthread_rwlock_unlock(&cache->lock);
        return false;
    }

    time_t now = time(NULL);
    if (entry->expires_at > 0 && now >= entry->expires_at) {
        *stale_found = true;
        cache_evict(cache, entry);
        pthread_rwlock_unlock(&cache->lock);
        return false;
    }

    char *copy = malloc(entry->response_data_len);
    if (!copy) {
        pthread_rwlock_unlock(&cache->lock);
        return false;
    }
    memcpy(copy, entry->response_data, entry->response_data_len);
    *response_copy = copy;
    *response_len = entry->response_data_len;
    pthread_rwlock_unlock(&cache->lock);
    return true;
}

void handle_request(int conn_fd, LRUCache *cache, bool cache_enabled) {
    char last_header_line[MAX_REQUEST_LINE_SIZE] = "";
    char *request_buffer = NULL;
    char *full_request_headers = NULL;
    size_t total_request_headers_bytes = 0;
    char *prefetched_request_body = NULL;
    size_t prefetched_request_body_len = 0;
    char *host = NULL;
    char *clean_host = NULL;
    int origin_server_fd = -1;
    char *response_headers = NULL;
    char *complete_response = NULL;
    char *cached_response_copy = NULL;
    size_t cached_response_len = 0;
    bool stale_found = false;
    bool request_ok = false;

    if (!read_request_headers_with_state_machine(conn_fd,
                                                 &request_buffer,
                                                 &total_request_headers_bytes,
                                                 &prefetched_request_body,
                                                 &prefetched_request_body_len,
                                                 last_header_line,
                                                 sizeof(last_header_line))) {
        goto cleanup;
    }
    full_request_headers = request_buffer;

    // 2. log request tail
    log_request_tail(last_header_line);


    // 3. parse the request line
    char method[MAX_METHOD_SIZE];
    char uri[MAX_URI_SIZE];
    char version[MAX_VERSION_SIZE];
    char *first_line_end = strstr(full_request_headers, "\r\n");
    if (!first_line_end) {
        print_log("Invalid request line");
        goto cleanup;
    }
    size_t first_line_len = first_line_end - full_request_headers;

    if (first_line_len >= MAX_REQUEST_LINE_SIZE) {
        print_log("Request line too long: %zu bytes, max allowed: %d bytes", first_line_len, MAX_REQUEST_LINE_SIZE - 1);
        goto cleanup;
    }

    char first_request_line[MAX_REQUEST_LINE_SIZE];
    strncpy(first_request_line, full_request_headers, first_line_len);
    first_request_line[first_line_len] = '\0';


    parse_request_line(first_request_line, method, uri, version);

    // 4. parse host header
    host = get_header_value(full_request_headers, "Host");
    if (!host) {
        print_log("Host header not found");
        goto cleanup;
    }

    // 5. Check cache if request is small enough
    if (cache_enabled && total_request_headers_bytes < MAX_CACHE_KEY_SIZE) {
        if (cache_get_response_copy(cache, full_request_headers, &cached_response_copy, &cached_response_len, &stale_found)) {
            // Serve from cache
            log_serving_from_cache(host, uri);
            if (send_all(conn_fd, cached_response_copy, cached_response_len) == -1) {
                perror("send cached response to client");
            }
            request_ok = true;
            goto cleanup;
        }
        if (stale_found) {
            log_stale_entry(host, uri);
        }
    }

    // 6. log getting
    log_getting(host, uri);

    // strip IPv6 brackets for getaddrinfo
    clean_host = strip_ipv6_brackets(host);
    if (!clean_host) {
        goto cleanup;
    }

    // 7. connect to origin server
    struct addrinfo hints, *servinfo, *p;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // Allow both IPv4 and IPv6
    hints.ai_socktype = SOCK_STREAM;

    // port 80 is used for HTTP
    if (getaddrinfo(clean_host, "80", &hints, &servinfo) != 0) {
        perror("getaddrinfo");
        goto cleanup;
    }

    // try each address until we connect successfully
    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((origin_server_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            continue;
        }
        if (connect(origin_server_fd, p->ai_addr, p->ai_addrlen) == -1) {
            close(origin_server_fd);
            origin_server_fd = -1;
            continue;
        }
        break; // success
    }
    freeaddrinfo(servinfo);

    if (origin_server_fd == -1) {
        print_log("Failed to connect to origin server");
        goto cleanup; // discard request
    }

    // forward request headers/body and then handle response
    if (send_all(origin_server_fd, full_request_headers, total_request_headers_bytes) == -1) {
        perror("send to origin server");
        goto cleanup;
    }

    // If recv() already got body bytes together with headers, forward them too.
    size_t body_forwarded = 0;
    long request_content_length = parse_content_length_header(full_request_headers);
    if (prefetched_request_body_len > 0) {
        if (send_all(origin_server_fd, prefetched_request_body, prefetched_request_body_len) == -1) {
            perror("send prefetched body to origin");
            goto cleanup;
        }
        body_forwarded = prefetched_request_body_len;
    }

    while (request_content_length >= 0 && body_forwarded < (size_t)request_content_length) {
        char body_buf[BUFFER_SIZE];
        size_t remaining = (size_t)request_content_length - body_forwarded;
        size_t to_read = remaining < sizeof(body_buf) ? remaining : sizeof(body_buf);
        ssize_t body_n = recv(conn_fd, body_buf, to_read, 0);
        if (body_n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv request body from client");
            goto cleanup;
        }
        if (body_n == 0) {
            print_log("Client closed connection before sending full request body");
            goto cleanup;
        }
        if (send_all(origin_server_fd, body_buf, (size_t)body_n) == -1) {
            perror("forward request body to origin");
            goto cleanup;
        }
        body_forwarded += (size_t)body_n;
    }

    // read and forward response
    char response_buffer[BUFFER_SIZE];
    ssize_t response_bytes = 0;
    long content_length = -1;
    bool headers_complete = false;
    size_t headers_size = 0; // Need this to track cumulative header size
    bool content_length_logged = false;
    size_t body_bytes_received = 0; // Track received body bytes
    bool headers_too_large = false;
    bool client_send_failed = false;
    bool response_complete = false;
    bool cache_capture_enabled = cache_enabled && total_request_headers_bytes < MAX_CACHE_KEY_SIZE;

    // Buffer for complete response (for caching)
    size_t complete_response_size = 0;
    size_t complete_response_capacity = 0;

    while ((response_bytes = recv(origin_server_fd, response_buffer, sizeof(response_buffer), 0)) > 0) {
        // Send data to client first
        if (send_all(conn_fd, response_buffer, (size_t)response_bytes) == -1) {
            perror("send to client");
            client_send_failed = true;
            break;
        }

        // Buffer response for potential caching (only if request is cacheable)
        if (cache_capture_enabled) {
            if (complete_response_size + response_bytes > complete_response_capacity) {
                size_t new_capacity = complete_response_capacity == 0 ? BUFFER_SIZE : complete_response_capacity * 2;
                while (new_capacity < complete_response_size + response_bytes) {
                    new_capacity *= 2;
                }
                // Don't allocate more than MAX_CACHE_VALUE_SIZE
                if (new_capacity > MAX_CACHE_VALUE_SIZE) {
                    new_capacity = MAX_CACHE_VALUE_SIZE;
                }
                
                char *new_buffer = realloc(complete_response, new_capacity);
                if (new_buffer && complete_response_size + response_bytes <= new_capacity) {
                    complete_response = new_buffer;
                    complete_response_capacity = new_capacity;
                } else {
                    // Can't expand buffer or would exceed limit, stop caching
                    free(complete_response);
                    complete_response = NULL;
                    complete_response_size = 0;
                    complete_response_capacity = 0;
                    cache_capture_enabled = false;
                }
            }
            
            if (complete_response && complete_response_size + response_bytes <= complete_response_capacity) {
                memcpy(complete_response + complete_response_size, response_buffer, response_bytes);
                complete_response_size += response_bytes;
            }
        }

        if (!headers_complete && !headers_too_large) {
            // accumulate headers until we find the end
            if (headers_size + (size_t)response_bytes > MAX_RESPONSE_HEADERS_SIZE) {
                headers_too_large = true;
                headers_complete = true;
                free(response_headers);
                response_headers = NULL;
            } else {
            char *new_headers = realloc(response_headers, headers_size + response_bytes + 1);
            if (!new_headers) {
                    perror("realloc response headers failed");
                    headers_too_large = true;
                    headers_complete = true;
                    free(response_headers);
                    response_headers = NULL;
                } else {
                    response_headers = new_headers;
                    memcpy(response_headers + headers_size, response_buffer, response_bytes);
                    headers_size += response_bytes;
                    response_headers[headers_size] = '\0';

                    char *headers_end = strstr(response_headers, "\r\n\r\n");
                    if (headers_end) {
                        headers_complete = true;
                        // parse Content-Length only once
                        if (!content_length_logged) {
                            content_length = parse_content_length_header(response_headers);
                            if (content_length >= 0) {
                                log_response_body_length(content_length);
                                content_length_logged = true;
                            }
                        }
                        
                        // Calculate how many body bytes we've already received in this buffer
                        size_t headers_total_size = headers_end + 4 - response_headers;
                        size_t body_in_first_buffer = headers_size - headers_total_size;
                        body_bytes_received += body_in_first_buffer;
                    }
                }
            }
        } else {
            // Headers already complete, this is pure body data
            body_bytes_received += response_bytes;
        }
        
        // Check if we've received all expected body data
        if (content_length >= 0 && body_bytes_received >= (size_t)content_length) {
            response_complete = true;
            break; // Stop receiving when we have all the expected data
        }
    }

    if (response_bytes == 0 && !client_send_failed) {
        if (content_length >= 0) {
            response_complete = (body_bytes_received >= (size_t)content_length);
        } else {
            response_complete = true;
        }
    } else if (response_bytes < 0) {
        perror("recv from origin");
    }

    // Add to cache if conditions are met
    if (response_complete && !client_send_failed &&
        complete_response && complete_response_size > 0 &&
        cache_enabled && total_request_headers_bytes < MAX_CACHE_KEY_SIZE &&
        !headers_too_large && response_headers &&
        complete_response_size <= MAX_CACHE_VALUE_SIZE) {
        
        // Check if response is cacheable based on Cache-Control header
        if (is_cacheable_response(response_headers)) {
            // Normal cache put
            pthread_rwlock_wrlock(&cache->lock);
            cache_put(cache, full_request_headers, total_request_headers_bytes,
                      complete_response, complete_response_size, host, uri, content_length,
                      response_headers);
            pthread_rwlock_unlock(&cache->lock);
        } else {
            // Response has Cache-Control directives that prevent caching
            log_not_caching(host, uri);
        }
    }

    request_ok = true;

cleanup:
    free(response_headers);
    free(complete_response);
    free(cached_response_copy);
    free(host);
    free(clean_host);
    free(request_buffer);
    if (origin_server_fd >= 0) {
        close(origin_server_fd);
    }
    (void)request_ok;
}



// Initialize cache
void cache_init(LRUCache *cache, int capacity) {
    cache->head = NULL;
    cache->tail = NULL;
    cache->bucket_count = CACHE_HASH_BUCKETS;
    cache->buckets = calloc(cache->bucket_count, sizeof(CacheEntry *));
    if (!cache->buckets) {
        cache->bucket_count = 0;
    }
    pthread_rwlock_init(&cache->lock, NULL);
    cache->count = 0;
    cache->capacity = capacity;
}

static size_t cache_hash_key(const char *key, size_t bucket_count) {
    unsigned long hash = 5381;
    const unsigned char *p = (const unsigned char *)key;
    while (*p) {
        hash = ((hash << 5) + hash) + *p;
        p++;
    }
    return bucket_count > 0 ? (size_t)(hash % bucket_count) : 0;
}

static CacheEntry *cache_hash_find(LRUCache *cache, const char *request_key) {
    if (!cache || !cache->buckets || cache->bucket_count == 0) {
        return NULL;
    }
    size_t idx = cache_hash_key(request_key, cache->bucket_count);
    CacheEntry *entry = cache->buckets[idx];
    while (entry) {
        if (strcmp(entry->request_key, request_key) == 0) {
            return entry;
        }
        entry = entry->hash_next;
    }
    return NULL;
}

static void cache_hash_insert(LRUCache *cache, CacheEntry *entry) {
    if (!cache || !entry || !cache->buckets || cache->bucket_count == 0) {
        return;
    }
    size_t idx = cache_hash_key(entry->request_key, cache->bucket_count);
    entry->hash_next = cache->buckets[idx];
    cache->buckets[idx] = entry;
}

static void cache_hash_remove(LRUCache *cache, CacheEntry *entry) {
    if (!cache || !entry || !cache->buckets || cache->bucket_count == 0 || !entry->request_key) {
        return;
    }
    size_t idx = cache_hash_key(entry->request_key, cache->bucket_count);
    CacheEntry **cur = &cache->buckets[idx];
    while (*cur) {
        if (*cur == entry) {
            *cur = entry->hash_next;
            entry->hash_next = NULL;
            return;
        }
        cur = &((*cur)->hash_next);
    }
}

// Detach a node from the linked list
void cache_detach_node(LRUCache *cache, CacheEntry *entry) {
    if (!entry) return;
    
    if (entry->prev) {
        entry->prev->next = entry->next;
    } else {
        // entry is head
        cache->head = entry->next;
    }
    
    if (entry->next) {
        entry->next->prev = entry->prev;
    } else {
        // entry is tail
        cache->tail = entry->prev;
    }
    
    entry->prev = NULL;
    entry->next = NULL;
}

// Attach a node to the head of the linked list
void cache_attach_to_head(LRUCache *cache, CacheEntry *entry) {
    if (!entry) return;
    
    entry->prev = NULL;
    entry->next = cache->head;
    
    if (cache->head) {
        cache->head->prev = entry;
    } else {
        // Cache was empty
        cache->tail = entry;
    }
    
    cache->head = entry;
}

// Get an entry from cache (and move to head if found)
CacheEntry *cache_get(LRUCache *cache, const char *request_key) {
    CacheEntry *current = cache_hash_find(cache, request_key);
    if (!current) {
        current = cache->head;
        while (current) {
            if (strcmp(current->request_key, request_key) == 0) {
                break;
            }
            current = current->next;
        }
    }

    if (current) {
        // Found matching entry, move to head (mark as recently used)
        cache_detach_node(cache, current);
        cache_attach_to_head(cache, current);
        return current;
    }

    return NULL; // Not found
}

// Evict an entry from cache
void cache_evict(LRUCache *cache, CacheEntry *entry_to_evict) {
    // If entry_to_evict is NULL, evict LRU item (tail)
    if (!entry_to_evict) {
        entry_to_evict = cache->tail;
    }
    
    // If still no entry to evict (cache is empty), return
    if (!entry_to_evict) {
        return;
    }
    
    // Log eviction
    log_evicting_from_cache(entry_to_evict->host_for_log, entry_to_evict->uri_for_log);
    
    // Remove from linked list
    cache_detach_node(cache, entry_to_evict);
    cache_hash_remove(cache, entry_to_evict);
    
    // Free memory
    free(entry_to_evict->request_key);
    free(entry_to_evict->response_data);
    free(entry_to_evict->host_for_log);
    free(entry_to_evict->uri_for_log);
    free(entry_to_evict);
    
    cache->count--;
}

// Simplified function to evict LRU entry
void cache_evict_lru(LRUCache *cache) {
    cache_evict(cache, NULL);  // NULL means evict LRU (tail)
}

// Clean up entire cache, free all memory
void cache_cleanup(LRUCache *cache) {
    if (!cache) return;
    
    CacheEntry *current = cache->head;
    while (current) {
        CacheEntry *next = current->next;
        
        // Free all memory for this entry
        free(current->request_key);
        free(current->response_data);
        free(current->host_for_log);
        free(current->uri_for_log);
        free(current);
        
        current = next;
    }
    
    // Reset cache state
    cache->head = NULL;
    cache->tail = NULL;
    cache->count = 0;
    free(cache->buckets);
    cache->buckets = NULL;
    cache->bucket_count = 0;
    pthread_rwlock_destroy(&cache->lock);
}

// Put an entry into cache
void cache_put(LRUCache *cache, const char *request_key, size_t request_key_len, 
               const char *response_data, size_t response_data_len, 
               const char *host, const char *uri, long response_body_actual_len,
               const char *response_headers) {
    
    // Condition checks
    if (request_key_len >= MAX_CACHE_KEY_SIZE) {
        return; // Request too large, don't cache
    }
    
    if (response_body_actual_len > MAX_CACHE_VALUE_SIZE) {
        return; // Response too large, don't cache
    }

    CacheEntry *existing = cache_hash_find(cache, request_key);
    if (existing) {
        cache_replace_entry(cache, existing, request_key, request_key_len, response_data,
                            response_data_len, host, uri, response_body_actual_len, response_headers);
        return;
    }
    
    // If cache is full, evict LRU entry
    if (cache->count >= cache->capacity) {
        cache_evict_lru(cache);
    }
    
    // Create new entry
    CacheEntry *new_entry = (CacheEntry *)malloc(sizeof(CacheEntry));
    if (!new_entry) return;
    
    new_entry->request_key = strdup(request_key);
    new_entry->response_data = (char *)malloc(response_data_len);
    if (!new_entry->response_data) {
        free(new_entry->request_key);
        free(new_entry);
        return;
    }
    
    memcpy(new_entry->response_data, response_data, response_data_len);
    new_entry->response_data_len = response_data_len;
    new_entry->host_for_log = strdup(host);
    new_entry->uri_for_log = strdup(uri);
    new_entry->hash_next = NULL;
    
    // Calculate expiration time
    new_entry->expires_at = 0; // Default: no expiration
    if (response_headers) {
        char *cache_control = get_header_value(response_headers, "Cache-Control");
        if (cache_control) {
            uint32_t max_age = parse_max_age(cache_control);
            if (max_age > 0) {
                new_entry->expires_at = time(NULL) + max_age;
            }
            free(cache_control);
        }
    }
    
    if (!new_entry->request_key || !new_entry->host_for_log || !new_entry->uri_for_log) {
        // Memory allocation failed, clean up
        free(new_entry->request_key);
        free(new_entry->response_data);
        free(new_entry->host_for_log);
        free(new_entry->uri_for_log);
        free(new_entry);
        return;
    }
    
    // Add to head of linked list
    cache_attach_to_head(cache, new_entry);
    cache_hash_insert(cache, new_entry);
    cache->count++;
}

// Replace an existing stale entry with fresh data
void cache_replace_entry(LRUCache *cache, CacheEntry *cached_entry, const char *request_key, size_t request_key_len,
                        const char *response_data, size_t response_data_len, 
                        const char *host, const char *uri, long response_body_actual_len,
                        const char *response_headers) {
    (void)request_key_len;
    (void)response_body_actual_len;

    char *new_request_key = strdup(request_key);
    char *new_response_data = (char *)malloc(response_data_len);
    char *new_host = strdup(host);
    char *new_uri = strdup(uri);
    time_t new_expires_at = 0;

    if (!new_request_key || !new_response_data || !new_host || !new_uri) {
        free(new_request_key);
        free(new_response_data);
        free(new_host);
        free(new_uri);
        return;
    }
    memcpy(new_response_data, response_data, response_data_len);

    if (response_headers) {
        char *cache_control = get_header_value(response_headers, "Cache-Control");
        if (cache_control) {
            uint32_t max_age = parse_max_age(cache_control);
            if (max_age > 0) {
                new_expires_at = time(NULL) + max_age;
            }
            free(cache_control);
        }
    }

    cache_hash_remove(cache, cached_entry);
    free(cached_entry->request_key);
    free(cached_entry->response_data);
    free(cached_entry->host_for_log);
    free(cached_entry->uri_for_log);

    cached_entry->request_key = new_request_key;
    cached_entry->response_data = new_response_data;
    cached_entry->response_data_len = response_data_len;
    cached_entry->host_for_log = new_host;
    cached_entry->uri_for_log = new_uri;
    cached_entry->expires_at = new_expires_at;
    cached_entry->hash_next = NULL;

    cache_hash_insert(cache, cached_entry);

    // Move to head (mark as recently used)
    cache_detach_node(cache, cached_entry);
    cache_attach_to_head(cache, cached_entry);
}