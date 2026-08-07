#include <stdatomic.h>
#include <stdio.h>

#include "stats.h"

static network_stats_t g_stats = {0};

void stats_record_connection_accepted(void)
{
    atomic_fetch_add(&g_stats.connections_accepted, 1);
}

void stats_record_connection_closed(void)
{
    atomic_fetch_add(&g_stats.connections_closed, 1);
}

void stats_record_bytes_received(uint64_t n)
{
    atomic_fetch_add(&g_stats.bytes_received, n);
    atomic_fetch_add(&g_stats.packets_received, 1);
}

void stats_record_bytes_sent(uint64_t n)
{
    atomic_fetch_add(&g_stats.bytes_sent, n);
    atomic_fetch_add(&g_stats.packets_sent, 1);
}

void stats_record_error(void)
{
    atomic_fetch_add(&g_stats.errors, 1);
}

int stats_format_json(char *buf, size_t size)
{
    int n = snprintf(buf, size,
                     "{"
                     "\"bytes_sent\":%llu,"
                     "\"bytes_received\":%llu,"
                     "\"packets_sent\":%llu,"
                     "\"packets_received\":%llu,"
                     "\"connections_accepted\":%llu,"
                     "\"connections_closed\":%llu,"
                     "\"errors\":%llu"
                     "}",
                     (unsigned long long)atomic_load(&g_stats.bytes_sent),
                     (unsigned long long)atomic_load(&g_stats.bytes_received),
                     (unsigned long long)atomic_load(&g_stats.packets_sent),
                     (unsigned long long)atomic_load(&g_stats.packets_received),
                     (unsigned long long)atomic_load(&g_stats.connections_accepted),
                     (unsigned long long)atomic_load(&g_stats.connections_closed),
                     (unsigned long long)atomic_load(&g_stats.errors));

    if (n < 0 || (size_t)n >= size)
    {
        return -1;
    }
    return n;
}
