#ifndef CONFIG_H
#define CONFIG_H

#define MAX_EVENTS 10000
#define MAX_CONNECTIONS 100000
#define BUFFER_SIZE 8192
#define MAX_REQUEST_SIZE 65536
#define MAX_RESPONSE_SIZE 1048576
#define WORKER_THREADS 8
// Threads dedicated to blocking, storage-backed handlers. Sized above the core
// count on purpose: these threads spend most of their time parked in fsync/
// pread/pwrite, so oversubscription keeps the CPU busy while they block.
#define STORAGE_POOL_THREADS 16
#define BACKLOG 1024
#define KEEPALIVE_TIMEOUT 30
#define MAX_HEADERS 64
#define MAX_HEADERS_SIZE 8192

#endif
