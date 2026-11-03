#ifndef MYCP_H
#define MYCP_H

#include <stddef.h>
#include <sys/types.h>

#define DEFAULT_BUF_SIZE 4096
#define MAX_BUF_SIZE     (256UL * 1024 * 1024)
#define MAX_THREADS      64

/* Command-line options shared by all modules */
typedef struct {
    size_t      buf_size;    /* Level 2: buffer size in bytes       */
    int         show_info;   /* Level 3: file information display   */
    int         progress;    /* Level 4: progress bar               */
    int         preserve;    /* Level 5: preserve permissions       */
    int         recursive;   /* Ext: recursive directory copy       */
    int         benchmark;   /* Ext: copy performance benchmark     */
    int         verify;      /* Ext: copy verification              */
    int         resume;      /* Ext: resume interrupted copy        */
    int         threads;     /* Ext: parallel copy (pthreads)       */
    const char *log_path;    /* Ext: logging system (NULL = off)    */
} options_t;

/* Performance statistics */
typedef struct {
    unsigned long long bytes;
    double             seconds;
    unsigned long      files_ok;
    unsigned long      files_failed;
} stats_t;

/* copy.c - single file copy (Levels 1-5, verify, resume) */
int copy_file(const char *src, const char *dst,
              const options_t *opt, stats_t *stats);

/* dircopy.c - recursive copy with job queue and pthreads */
int copy_directory(const char *src, const char *dst,
                   const options_t *opt, stats_t *stats);

/* utils.c - I/O helpers */
int     write_all(int fd, const void *buf, size_t count);
ssize_t read_full(int fd, void *buf, size_t count);
int     parse_size(const char *text, size_t *out);
void    format_size(unsigned long long bytes, char *out, size_t cap);
double  now_seconds(void);
int     join_path(char *out, size_t cap, const char *dir, const char *name);
const char *base_name(const char *path);

/* utils.c - logging system */
int  log_open(const char *path);
void log_close(void);
void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void report_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
