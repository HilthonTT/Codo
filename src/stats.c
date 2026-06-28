#define _GNU_SOURCE

#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>

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

void update_connection_rtt(connection_stats_t *stats, uint32_t rtt_us)
{
    stats->rtt_samples[stats->rtt_index++ % 100] = rtt_us;
}

uint32_t get_average_rtt(connection_stats_t *stats)
{
    uint64_t sum = 0;
    int count = (stats->rtt_index < 100) ? stats->rtt_index : 100;

    for (int i = 0; i < count; i++)
    {
        sum += stats->rtt_samples[i];
    }

    return count > 0 ? sum / count : 0;
}

// Packet capture for debugging
void debug_packet_dump(const uint8_t *data, size_t len)
{
    printf("Packet dump (%zu bytes):\n", len);

    for (size_t i = 0; i < len; i += 16)
    {
        printf("%04zx: ", i);

        // Hex dump
        for (size_t j = 0; j < 16; j++)
        {
            if (i + j < len)
            {
                printf("%02x ", data[i + j]);
            }
            else
            {
                printf("   ");
            }
            if (j == 7)
                printf(" ");
        }

        printf(" |");

        // ASCII dump
        for (size_t j = 0; j < 16 && i + j < len; j++)
        {
            uint8_t c = data[i + j];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }

        printf("|\n");
    }
}

// Network diagnostic tool
void diagnose_network_issue(int sock)
{
    // Get socket error
    int error;
    socklen_t len = sizeof(error);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);

    if (error != 0)
    {
        printf("Socket error: %s\n", strerror(error));
    }

    // Get TCP info
    struct tcp_info info;
    len = sizeof(info);
    if (getsockopt(sock, IPPROTO_TCP, TCP_INFO, &info, &len) == 0)
    {
        printf("TCP diagnostics:\n");
        printf("  State: %u\n", info.tcpi_state);
        printf("  Retransmits: %u\n", info.tcpi_retransmits);
        printf("  Lost packets: %u\n", info.tcpi_lost);
        printf("  Reordering: %u\n", info.tcpi_reordering);
        printf("  RTT: %u us (variance: %u)\n",
               info.tcpi_rtt, info.tcpi_rttvar);
        printf("  Send buffer: %u bytes\n", info.tcpi_snd_ssthresh);
        printf("  Congestion window: %u\n", info.tcpi_snd_cwnd);
    }

    // Check system limits
    struct rlimit rlim;
    getrlimit(RLIMIT_NOFILE, &rlim);
    printf("File descriptor limit: %lu (max: %lu)\n",
           rlim.rlim_cur, rlim.rlim_max);

    // Check network buffers
    FILE *fp = fopen("/proc/net/sockstat", "r");
    if (fp)
    {
        char line[256];
        while (fgets(line, sizeof(line), fp))
        {
            printf("  %s", line);
        }
        fclose(fp);
    }
}
