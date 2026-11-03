/* utils.c - I/O helpers, size/time/path helpers and the logging system */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#include "mycp.h"

/* ---------- I/O helpers ---------- */

/* Write exactly 'count' bytes (handles partial writes and EINTR). */
int write_all(int fd, const void *buf, size_t count)
{
    const char *p = buf;
    size_t done = 0;

    while (done < count) {
        ssize_t n = write(fd, p + done, count - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/* Read until 'count' bytes or EOF. */
ssize_t read_full(int fd, void *buf, size_t count)
{
    char *p = buf;
    size_t got = 0;

    while (got < count) {
        ssize_t n = read(fd, p + got, count - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    return (ssize_t)got;
}

/* ---------- size / time / path helpers ---------- */

/* "4096", "64K", "1M" -> bytes. Returns 0 / -1. */
int parse_size(const char *text, size_t *out)
{
    char *end;
    unsigned long long v;

    if (text[0] < '0' || text[0] > '9')
        return -1;
    errno = 0;
    v = strtoull(text, &end, 10);
    if (errno != 0 || v == 0 || v > MAX_BUF_SIZE)
        return -1;

    if (*end != '\0') {
        if (end[1] != '\0')
            return -1;
        if (*end == 'k' || *end == 'K')
            v *= 1024ULL;
        else if (*end == 'm' || *end == 'M')
            v *= 1024ULL * 1024ULL;
        else
            return -1;
    }
    if (v > MAX_BUF_SIZE)
        return -1;
    *out = (size_t)v;
    return 0;
}

void format_size(unsigned long long bytes, char *out, size_t cap)
{
    static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;

    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(out, cap, "%llu B", bytes);
    else
        snprintf(out, cap, "%.2f %s", v, units[u]);
}

double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int join_path(char *out, size_t cap, const char *dir, const char *name)
{
    size_t len = strlen(dir);
    int n = snprintf(out, cap, (len > 0 && dir[len - 1] == '/') ? "%s%s" : "%s/%s",
                     dir, name);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* ---------- logging system (thread-safe, append mode) ---------- */

static int             g_fd = -1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_line(const char *level, const char *msg)
{
    char ts[32], line[2048];
    struct tm tm;
    time_t now;
    int n;

    if (g_fd < 0)
        return;
    now = time(NULL);
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    n = snprintf(line, sizeof(line), "[%s] %-5s %s\n", ts, level, msg);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(line)) {
        n = (int)sizeof(line) - 1;
        line[n - 1] = '\n';
    }
    pthread_mutex_lock(&g_lock);
    write_all(g_fd, line, (size_t)n);
    pthread_mutex_unlock(&g_lock);
}

int log_open(const char *path)
{
    g_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_fd < 0) {
        fprintf(stderr, "mycp: cannot open log file '%s': %s\n",
                path, strerror(errno));
        return -1;
    }
    return 0;
}

void log_close(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

void log_info(const char *fmt, ...)
{
    char msg[1536];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    log_line("INFO", msg);
}

/* Print "mycp: <msg>" to stderr and log it as ERROR. */
void report_error(const char *fmt, ...)
{
    char msg[1536];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "mycp: %s\n", msg);
    log_line("ERROR", msg);
}
