FROM ubuntu:22.04

# Install only C dependencies
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -yy --no-install-recommends \
        gcc \
        make \
        valgrind \
        curl \
        net-tools \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /project
COPY . /project

# Build the proxy
RUN make

# Expose the proxy port (default 8080)
EXPOSE 8080

# Default command: run proxy on port 8080 with caching enabled
CMD ["./htproxy", "-p", "8080", "-c"]