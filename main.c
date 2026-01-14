#include "proxy.h"

int main(int argc, char *argv[]) {
    int listen_fd, conn_fd;
    struct sockaddr_in6 server_addr; // IPv6 allows for IPv4 too
    struct sockaddr_storage client_addr;
    bool cache_enabled = false;
    socklen_t sin_size;
    int port;

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

    while (1) {
        sin_size = sizeof(client_addr);
        if ((conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &sin_size)) == -1) {
            perror("accept");
            continue;
        }

        log_accepted();

        // handle client request
        handle_request(conn_fd, &cache, cache_enabled);

        // close connection
        close(conn_fd);
    }

    // close listening socket
    close(listen_fd);

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

void handle_request(int conn_fd, LRUCache *cache, bool cache_enabled) {
    char request_buffer[MAX_REQUEST_HEADERS_SIZE];
    char last_header_line[MAX_REQUEST_LINE_SIZE] = "";
    ssize_t bytes_read;
    int total_request_headers_bytes = 0;
    char *full_request_headers = NULL;

    // 1. read HTTP request headers from client
    memset(request_buffer, 0, MAX_REQUEST_HEADERS_SIZE);
    char *current_pos = request_buffer;
    size_t remaining_space = MAX_REQUEST_HEADERS_SIZE - 1; // -1 for null terminator

    while ((bytes_read = recv(conn_fd, current_pos, remaining_space, 0)) > 0) {
        total_request_headers_bytes += bytes_read;
        current_pos[bytes_read] = '\0'; // null terminate the string

        char *header_end_marker = strstr(request_buffer, "\r\n\r\n");
        if (header_end_marker) {
            // find the end of headers
            total_request_headers_bytes = header_end_marker + 4 - request_buffer;
            request_buffer[total_request_headers_bytes] = '\0'; // ensure null termination at end of headers
            full_request_headers = request_buffer;

            // find the last header line (the line just before \r\n\r\n)
            // work backwards from \r\n\r\n to find the start of the last line
            char *last_line_end = header_end_marker; 
            char *last_line_start;

            //the last header line ends right before the \r\n\r\n sequence
            last_line_start = last_line_end - 1;
            
            //find the start of the last line by working backwards
            while (last_line_start > request_buffer) {
                if (*last_line_start == '\n' && last_line_start > request_buffer && *(last_line_start - 1) == '\r') {
                    //find the end of previous line, so start of last line is right after
                    last_line_start++;
                    break;
                }
                last_line_start--;
            }

            //when reach the beginning, this is the first line (request line)
            if (last_line_start <= request_buffer) {
                last_line_start = request_buffer;
            }

            //extract the last header line
            size_t line_len = last_line_end - last_line_start;
            if (line_len > 0 && line_len < MAX_REQUEST_LINE_SIZE) {
                    strncpy(last_header_line, last_line_start, line_len);
                    last_header_line[line_len] = '\0';

            }
            break;
        }
        current_pos += bytes_read;
        remaining_space -= bytes_read;
        if (remaining_space <= 0) {
            print_log("Request headers too large, max size is %d bytes", MAX_REQUEST_HEADERS_SIZE);
            close(conn_fd);
            return;
        }
    }

    if (bytes_read < 0) {
        perror("recv request headers from client");
        close(conn_fd);
        return;
    }
    if (bytes_read == 0 && !full_request_headers) {
        print_log("Client closed connection before sending full request headers");
        close(conn_fd);
        return;
    }

    // 2. log request tail
    log_request_tail(last_header_line);


    // 3. parse the request line
    char method[MAX_METHOD_SIZE];
    char uri[MAX_URI_SIZE];
    char version[MAX_VERSION_SIZE];
    char *first_line_end = strstr(full_request_headers, "\r\n");
    if (!first_line_end) {
        print_log("Invalid request line");
        close(conn_fd);
        return;
    }
    size_t first_line_len = first_line_end - full_request_headers;

    if (first_line_len >= MAX_REQUEST_LINE_SIZE) {
        print_log("Request line too long: %zu bytes, max allowed: %d bytes", first_line_len, MAX_REQUEST_LINE_SIZE - 1);
        close(conn_fd);
        return;
    }

    char first_request_line[MAX_REQUEST_LINE_SIZE];
    strncpy(first_request_line, full_request_headers, first_line_len);
    first_request_line[first_line_len] = '\0';


    parse_request_line(first_request_line, method, uri, version);

    // 4. parse host header
    char *host = get_header_value(full_request_headers, "Host");
    if (!host) {
        print_log("Host header not found");
        close(conn_fd);
        return;
    }

    // 5. Check cache if request is small enough
    CacheEntry *cached_entry = NULL;
    bool is_stale = false;
    if (cache_enabled && total_request_headers_bytes < MAX_CACHE_KEY_SIZE) {
        cached_entry = cache_get(cache, full_request_headers);
        if (cached_entry) {
            // Check if the entry is stale
            time_t now = time(NULL);
            if (cached_entry->expires_at > 0 && now >= cached_entry->expires_at) {
                is_stale = true;
                log_stale_entry(host, uri);
            } else {
                // Serve from cache
                log_serving_from_cache(host, uri);
                if (send(conn_fd, cached_entry->response_data, cached_entry->response_data_len, 0) == -1) {
                    perror("send cached response to client");
                }
                free(host);
                close(conn_fd);
                return;
            }
        }
    }

    // Even if not caching, eviction should occur if cache is full
    if (cache_enabled && !cached_entry) {
        if (cache->count >= cache->capacity) {
            cache_evict_lru(cache);
        }
    }

    // 6. log getting
    log_getting(host, uri);

    // strip IPv6 brackets for getaddrinfo
    char *clean_host = strip_ipv6_brackets(host);
    if (!clean_host) {
        free(host);
        close(conn_fd);
        return;
    }

    // 7. connect to origin server
    int origin_server_fd = -1;
    struct addrinfo hints, *servinfo, *p;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // Allow both IPv4 and IPv6
    hints.ai_socktype = SOCK_STREAM;

    // port 80 is used for HTTP
    if (getaddrinfo(clean_host, "80", &hints, &servinfo) != 0) {
        perror("getaddrinfo");
        free(host);
        free(clean_host);
        close(conn_fd);
        return;
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
        free(host);
        free(clean_host);
        close(conn_fd);
        return; // discard request
    }

    // forward request and handle response
    if (send(origin_server_fd, full_request_headers, total_request_headers_bytes, 0) == -1) {
        perror("send to origin server");
        free(host);
        free(clean_host);
        close(origin_server_fd);
        close(conn_fd);
        return;
    }

    // read and forward response
    char response_buffer[BUFFER_SIZE];
    ssize_t response_bytes;
    int content_length = -1;
    int headers_complete = 0;
    char *response_headers = NULL;
    size_t headers_size = 0; // Need this to track cumulative header size
    int content_length_logged = 0;
    int body_bytes_received = 0; // Track received body bytes
    
    // Buffer for complete response (for caching)
    char *complete_response = NULL;
    size_t complete_response_size = 0;
    size_t complete_response_capacity = 0;

    while ((response_bytes = recv(origin_server_fd, response_buffer, sizeof(response_buffer), 0)) > 0) {
        // Send data to client first
        if (send(conn_fd, response_buffer, response_bytes, 0) == -1) {
            perror("send to client");
            break;
        }

        // Buffer response for potential caching (only if request is cacheable)
        if (cache_enabled && total_request_headers_bytes < MAX_CACHE_KEY_SIZE) {
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
                }
            }
            
            if (complete_response && complete_response_size + response_bytes <= complete_response_capacity) {
                memcpy(complete_response + complete_response_size, response_buffer, response_bytes);
                complete_response_size += response_bytes;
            }
        }

        if (!headers_complete) {
            // accumulate headers until we find the end
            char *new_headers = realloc(response_headers, headers_size + response_bytes + 1);
            if (!new_headers) {
                free(response_headers);
                free(complete_response);
                free(host);
                free(clean_host);
                close(origin_server_fd);
                close(conn_fd);
                return;
            }
            response_headers = new_headers;
            memcpy(response_headers + headers_size, response_buffer, response_bytes);
            headers_size += response_bytes;
            response_headers[headers_size] = '\0';

            char *headers_end = strstr(response_headers, "\r\n\r\n");
            if (headers_end) {
                headers_complete = 1;
                // parse Content-Length only once
                if (!content_length_logged) {
                    char *cl_header = strcasestr(response_headers, "Content-Length:");
                    if (cl_header) {
                        cl_header += 15; // skip "Content-Length:"
                        while (*cl_header == ' ') cl_header++; // skip spaces
                        content_length = atoi(cl_header);
                        log_response_body_length(content_length);
                        content_length_logged = 1;
                    }
                }
                
                // Calculate how many body bytes we've already received in this buffer
                size_t headers_total_size = headers_end + 4 - response_headers;
                size_t body_in_first_buffer = headers_size - headers_total_size;
                body_bytes_received += body_in_first_buffer;
            }
        } else {
            // Headers already complete, this is pure body data
            body_bytes_received += response_bytes;
        }
        
        // Check if we've received all expected body data
        if (content_length >= 0 && body_bytes_received >= content_length) {
            break; // Stop receiving when we have all the expected data
        }
    }

    // Add to cache if conditions are met
    if (complete_response && complete_response_size > 0 && 
        cache_enabled && total_request_headers_bytes < MAX_CACHE_KEY_SIZE &&
        complete_response_size <= MAX_CACHE_VALUE_SIZE) {
        
        // Check if response is cacheable based on Cache-Control header
        if (is_cacheable_response(response_headers)) {
            if (is_stale && cached_entry) {
                // Replace the stale entry directly
                cache_replace_entry(cache, cached_entry, full_request_headers, total_request_headers_bytes,
                                  complete_response, complete_response_size, host, uri, content_length,
                                  response_headers);
            } else {
                // Normal cache put
                cache_put(cache, full_request_headers, total_request_headers_bytes,
                          complete_response, complete_response_size, host, uri, content_length,
                          response_headers);
            }
        } else {
            // Response has Cache-Control directives that prevent caching
            log_not_caching(host, uri);
            // If we had a stale entry but can't cache new response, evict the stale one
            if (is_stale && cached_entry) {
                cache_evict(cache, cached_entry);
            }
        }
    } else if (is_stale && cached_entry) {
        // We had a stale entry but didn't cache the new response, so evict the stale one
        cache_evict(cache, cached_entry);
    }

    free(response_headers);
    free(complete_response);
    free(host);
    free(clean_host);
    close(origin_server_fd);
}



// Initialize cache
void cache_init(LRUCache *cache, int capacity) {
    cache->head = NULL;
    cache->tail = NULL;
    cache->count = 0;
    cache->capacity = capacity;
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
    CacheEntry *current = cache->head;
    
    while (current) {
        if (strcmp(current->request_key, request_key) == 0) {
            // Found matching entry, move to head (mark as recently used)
            cache_detach_node(cache, current);
            cache_attach_to_head(cache, current);
            return current;
        }
        current = current->next;
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
    cache->count++;
}

// Replace an existing stale entry with fresh data
void cache_replace_entry(LRUCache *cache, CacheEntry *cached_entry, const char *request_key, size_t request_key_len,
                        const char *response_data, size_t response_data_len, 
                        const char *host, const char *uri, long response_body_actual_len,
                        const char *response_headers) {
    
    // Reuse the existing entry structure, but replace its content
    free(cached_entry->request_key);
    free(cached_entry->response_data);
    free(cached_entry->host_for_log);
    free(cached_entry->uri_for_log);
    
    // Set new data
    cached_entry->request_key = strdup(request_key);
    cached_entry->response_data = (char *)malloc(response_data_len);
    if (!cached_entry->response_data) {
        // Memory allocation failed, evict the entry
        cache_evict(cache, cached_entry);
        return;
    }
    
    memcpy(cached_entry->response_data, response_data, response_data_len);
    cached_entry->response_data_len = response_data_len;
    cached_entry->host_for_log = strdup(host);
    cached_entry->uri_for_log = strdup(uri);
    
    // Calculate expiration time
    cached_entry->expires_at = 0; // Default: no expiration
    if (response_headers) {
        char *cache_control = get_header_value(response_headers, "Cache-Control");
        if (cache_control) {
            uint32_t max_age = parse_max_age(cache_control);
            if (max_age > 0) {
                cached_entry->expires_at = time(NULL) + max_age;
            }
            free(cache_control);
        }
    }
    
    if (!cached_entry->request_key || !cached_entry->host_for_log || !cached_entry->uri_for_log) {
        // Memory allocation failed, evict the entry
        cache_evict(cache, cached_entry);
        return;
    }
    
    // Move to head (mark as recently used)
    cache_detach_node(cache, cached_entry);
    cache_attach_to_head(cache, cached_entry);
}