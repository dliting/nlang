/* Probe: what does the byte-array element conversion at the native I/O
 * boundary actually cost?  The VM stores one 4-byte heap cell per byte
 * element, so _tcpRead scatters received bytes into int32 cells
 * (src/native/net/net_tcp.cpp:196-203) and _tcpWrite gathers them back
 * (net_tcp.cpp:287).  This probe times those two loops on an 8 KiB
 * window — the same window size as http.n's FILL_WINDOW — to bound the
 * share that design candidate P1 (packed byte payload) could reclaim.
 *
 * gcc -O2 mimics neither MSVC -O2 nor the VM's real call overhead; it
 * gives an order-of-magnitude upper bound for the loop alone.
 *
 * Run on this machine (POSIX clock, WSL2):
 *   gcc -O2 -Wall probe_byte_scatter.c -o /tmp/probe_scatter && /tmp/probe_scatter
 * Referenced by docs/dev/net_http_perf_design.md §2.4 and §九.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#define WINDOW 8192
#define ROUNDS 200000

static int32_t g_cells[WINDOW];
static uint8_t g_bytes[WINDOW];

static void unpack(int32_t *cells, const uint8_t *bytes, int count) {
    for (int i = 0; i < count; ++i)
        cells[i] = bytes[i];
}

static void pack(const int32_t *cells, uint8_t *bytes, int count) {
    for (int i = 0; i < count; ++i)
        bytes[i] = (uint8_t)cells[i];
}

static long long ns_since(struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (long long)(t1.tv_sec - t0->tv_sec) * 1000000000LL
         + (t1.tv_nsec - t0->tv_nsec);
}

int main(void) {
    long long acc_unpack = 0, acc_pack = 0, acc_null = 0, best_unpack = -1;
    volatile int32_t sink = 0;

    for (int i = 0; i < WINDOW; ++i) {
        g_bytes[i] = (uint8_t)(i & 0xff);
        g_cells[i] = i & 0xff;
    }
    for (int r = 0; r < ROUNDS; ++r) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        unpack(g_cells, g_bytes, 0);            /* timing overhead only */
        acc_null += ns_since(&t0);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        unpack(g_cells, g_bytes, WINDOW);
        long long u = ns_since(&t0);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        pack(g_cells, g_bytes, WINDOW);
        long long p = ns_since(&t0);
        acc_unpack += u;
        acc_pack += p;
        if (best_unpack < 0 || u < best_unpack) best_unpack = u;
        sink += g_cells[0];
    }
    printf("PROBE byte_scatter: window=%d rounds=%d\n", WINDOW, ROUNDS);
    printf("  empty-call overhead avg %.0f ns\n", (double)acc_null / ROUNDS);
    printf("  unpack avg %.0f ns (%.2f us), best %.0f ns\n",
           (double)acc_unpack / ROUNDS, (double)acc_unpack / ROUNDS / 1000.0,
           (double)best_unpack);
    printf("  pack   avg %.0f ns (%.2f us)\n",
           (double)acc_pack / ROUNDS, (double)acc_pack / ROUNDS / 1000.0);
    printf("  sink=%d\n", (int)sink);
    return 0;
}
