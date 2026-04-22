# HTTP Caching Proxy Service
![Language](https://img.shields.io/badge/language-C99-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Docker-orange.svg)
![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)

A robust HTTP/1.1 forward proxy server written in C. It acts as an intermediary between clients and origin servers, implementing a custom **LRU caching** engine and manual **TCP stream parsing**.

This project demonstrates low-level systems programming with **BSD sockets**, **POSIX threads**, custom data structures, and strict resource management.

## Key Features

- **Multithreaded producer-consumer model**: Uses a fixed `pthreads` worker pool to process accepted client sockets concurrently and remove single-threaded request handling bottlenecks.
- **Bounded TCP stream parser**: Parses request headers with a state machine over the byte stream (`\r\n\r\n` detection), handling sticky/half-packet cases with explicit buffer bounds.
- **Thread-safe LRU cache**: Implements **Hash Table + Doubly Linked List** for average `O(1)` lookup/update, including stale-entry eviction and cache replacement under a global cache lock.
- **HTTP cache semantics**: Respects `Cache-Control` directives (`private`, `no-store`, `no-cache`, `must-revalidate`) and `max-age` expiration behavior.
- **Dual-stack networking**: Built on raw **BSD sockets** with support for both **IPv4** and **IPv6** traffic (`struct sockaddr_in6`), handling address resolution via `getaddrinfo`.
- **Reliability hardening**: Masks `SIGPIPE`, performs explicit error-path cleanup (`malloc`/`free`, fd close), and only caches complete origin responses. No leaks were observed in Valgrind test runs.

## Tech Stack

- **Language**: C (Standard C99)
- **Concurrency**: POSIX Threads (`pthreads`), condition variables, lock-based cache synchronization
- **Networking**: BSD Sockets, TCP/IP
- **Tools**: Valgrind, GDB, cURL, hey
- **Containerization**: Docker

## System Architecture

```mermaid
graph TD
    Client[Client / Browser] -->|HTTP Request| Proxy[HTTP Proxy Server]
    Proxy -->|"accept()"| Queue[(Work Queue)]
    Queue -->|dispatch| Workers[Worker Pool (pthreads)]
    Workers -->|Lookup Key| Cache{LRU Cache (Hash+DLL)}
    
    Cache -->|Hit| Workers
    Workers -->|Cached Response| Client
    
    Cache -->|Miss| Origin[Origin Server]
    Origin -->|HTTP Response| Workers
    Workers -->|Update| Cache
    Workers -->|Forward Response| Client
    
    style Cache fill:#f9f,stroke:#333,stroke-width:2px
    style Workers fill:#bbf,stroke:#333,stroke-width:2px
```



## Project Structure

- `main.c`: Entry point, work queue + worker pool, request/response stream handling, and cache operations.
- `proxy.h`: Header file defining proxy constants, cache structures, and interfaces.
- `Makefile`: Build script to compile the `htproxy` executable.
- `Dockerfile`: Minimal environment for reproducible builds and testing.

## Usage

### 1) Build Locally

```bash
make
```

This generates the `htproxy` executable.

### 2) Run the Proxy

The proxy accepts a port number (`-p`) and an optional flag (`-c`) to enable caching:

```bash
# Syntax: ./htproxy -p <port> [-c]
./htproxy -p 8080 -c
```

### 3) Test with cURL

You can configure your browser to use the proxy, or test directly via command line:

```bash
# Send a request through the proxy (-x)
curl -v -x http://localhost:8080 http://www.example.com
```

### 4) Run with Docker

Build and run the proxy in an isolated container:

```bash
docker build -t http-proxy .
docker run -p 8080:8080 http-proxy
```

