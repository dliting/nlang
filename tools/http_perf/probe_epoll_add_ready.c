/* Probe: does epoll_ctl(EPOLL_CTL_ADD) of a file descriptor that is already
 * ready wake a thread that is blocked in epoll_wait()?  The VM design under
 * review registers park-for-IO interest from the VM thread while the poller
 * thread sits inside epoll_wait(), so the answer decides whether an explicit
 * wake channel (eventfd) is needed on POSIX.
 *
 * Case: a socketpair byte is already queued before the reading end is
 * registered; the waiter blocks in epoll_wait() with a 1000 ms timeout.  If
 * the ADD wakes it, the wait returns in milliseconds; if not, it returns only
 * at the timeout.
 *
 * Run on this machine (POSIX only, WSL2):
 *   gcc -O2 -Wall probe_epoll_add_ready.c -o /tmp/probe_add -lpthread && /tmp/probe_add
 * Referenced by docs/dev/net_http_perf_design.md §2.3 and §九.
 */
#include <stdio.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int g_epfd;
static long g_wait_ms;
static int g_events;

static void *waiter(void *arg) {
    struct epoll_event evs[4];
    struct timespec t0, t1;
    (void) arg;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    g_events = epoll_wait(g_epfd, evs, 4, 1000);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_wait_ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
    return NULL;
}

int main(void) {
    int sv[2];
    pthread_t th;
    struct epoll_event ev;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 2; }
    if (write(sv[1], "x", 1) != 1) { perror("write"); return 2; }

    g_epfd = epoll_create1(0);
    if (g_epfd < 0) { perror("epoll_create1"); return 2; }

    if (pthread_create(&th, NULL, waiter, NULL) != 0) { perror("pthread_create"); return 2; }
    usleep(100 * 1000);                       /* let the waiter enter epoll_wait */

    ev.events = EPOLLIN;
    ev.data.fd = sv[0];
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, sv[0], &ev) != 0) { perror("epoll_ctl"); return 2; }

    pthread_join(th, NULL);
    printf("PROBE epoll_add_ready: events=%d waited_ms=%ld -> %s\n",
           g_events, g_wait_ms,
           (g_events == 1 && g_wait_ms < 500) ? "WAKES_ON_ADD" : "NO_WAKE_TIMED_OUT");
    return 0;
}
