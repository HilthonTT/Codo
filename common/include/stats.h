#ifndef STATS_H
#define STATS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Network statistics collection
typedef struct
{
    _Atomic(uint64_t) bytes_sent;
    _Atomic(uint64_t) bytes_received;
    _Atomic(uint64_t) packets_sent;
    _Atomic(uint64_t) packets_received;
    _Atomic(uint64_t) connections_accepted;
    _Atomic(uint64_t) connections_closed;
    _Atomic(uint64_t) errors;
} network_stats_t;

// Per-connection statistics
typedef struct
{
    struct timespec connect_time;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint32_t rtt_samples[100];
    unsigned int rtt_index;
} connection_stats_t;

void update_connection_rtt(connection_stats_t *stats, uint32_t rtt_us);
uint32_t get_average_rtt(connection_stats_t *stats);
// Packet capture for debugging
void debug_packet_dump(const uint8_t *data, size_t len);
// Network diagnostic tool
void diagnose_network_issue(int sock);

// Global network statistics recorders (thread-safe; backed by atomics).
// bytes_* also bump the matching packets_* counter, so call once per read/write.
void stats_record_connection_accepted(void);
void stats_record_connection_closed(void);
void stats_record_bytes_received(uint64_t n);
void stats_record_bytes_sent(uint64_t n);
void stats_record_error(void);

// Serialize the global network stats as a JSON object into buf. Returns the
// number of bytes written (excluding the NUL terminator), or -1 on overflow.
int stats_format_json(char *buf, size_t size);

#endif
