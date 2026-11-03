/* dircopy.c - recursive directory copy.
 * Phase 1: walk the tree, create directories, put files in a linked-list queue.
 * Phase 2: worker threads (pthreads) take jobs from the queue and copy them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>

#include "mycp.h"

typedef struct job {                /* queue node: one file to copy */
    char       *src;
    char       *dst;
    struct job *next;
} job_t;

typedef struct dirnode {            /* directories to chmod at the end */
    char           *path;
    mode_t          mode;
    struct dirnode *next;
} dirnode_t;

typedef struct {
    const options_t   *opt;
    job_t             *head, *tail;     /* FIFO job queue            */
    size_t             queued;
    dirnode_t         *dirs;
    int                have_skip;       /* skip the destination dir  */
    dev_t              skip_dev;        /* if it lies inside source  */
    ino_t              skip_ino;
    pthread_mutex_t    lock;            /* protects queue + counters */
    unsigned long      files_ok, files_failed;
    unsigned long long bytes;
} ctx_t;

static int push_job(ctx_t *c, const char *src, const char *dst)
{
    job_t *j = malloc(sizeof(*j));

    if (!j)
        return -1;
    j->src = strdup(src);
    j->dst = strdup(dst);
    j->next = NULL;
    if (!j->src || !j->dst) {
        free(j->src);
        free(j->dst);
        free(j);
        return -1;
    }
    if (c->tail)
        c->tail->next = j;
    else
        c->head = j;
    c->tail = j;
    c->queued++;
    return 0;
}

static job_t *pop_job(ctx_t *c)
{
    job_t *j;

    pthread_mutex_lock(&c->lock);
    j = c->head;
    if (j) {
        c->head = j->next;
        if (!c->head)
            c->tail = NULL;
    }
    pthread_mutex_unlock(&c->lock);
    return j;
}

static int add_dir(ctx_t *c, const char *path, mode_t mode)
{
    dirnode_t *d = malloc(sizeof(*d));

    if (!d)
        return -1;
    d->path = strdup(path);
    if (!d->path) {
        free(d);
        return -1;
    }
    d->mode = mode;
    d->next = c->dirs;              /* newest first: children before parents */
    c->dirs = d;
    return 0;
}

/* Phase 1 */
static int walk(ctx_t *c, const char *src, const char *dst)
{
    struct stat sst, dstt;
    struct dirent *e;
    DIR *d;
    int rc = 0;

    if (stat(src, &sst) < 0) {
        report_error("cannot stat '%s': %s", src, strerror(errno));
        return -1;
    }
    if (mkdir(dst, 0755) < 0 && errno != EEXIST) {
        report_error("cannot create directory '%s': %s", dst, strerror(errno));
        return -1;
    }
    if (stat(dst, &dstt) < 0 || !S_ISDIR(dstt.st_mode)) {
        report_error("'%s' exists and is not a directory", dst);
        return -1;
    }
    if (!c->have_skip) {
        c->have_skip = 1;
        c->skip_dev = dstt.st_dev;
        c->skip_ino = dstt.st_ino;
    }
    if (c->opt->preserve && add_dir(c, dst, sst.st_mode) < 0) {
        report_error("out of memory");
        return -1;
    }
    d = opendir(src);
    if (!d) {
        report_error("cannot open directory '%s': %s", src, strerror(errno));
        return -1;
    }

    while ((e = readdir(d)) != NULL) {
        char sp[PATH_MAX], dp[PATH_MAX];
        struct stat est;

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (join_path(sp, sizeof(sp), src, e->d_name) < 0 ||
            join_path(dp, sizeof(dp), dst, e->d_name) < 0) {
            report_error("path too long under '%s'", src);
            rc = -1;
            continue;
        }
        if (lstat(sp, &est) < 0) {
            report_error("cannot stat '%s': %s", sp, strerror(errno));
            rc = -1;
            continue;
        }
        if (est.st_dev == c->skip_dev && est.st_ino == c->skip_ino)
            continue;                           /* destination inside source */

        if (S_ISDIR(est.st_mode)) {
            if (walk(c, sp, dp) < 0)
                rc = -1;
        } else if (S_ISREG(est.st_mode)) {
            if (push_job(c, sp, dp) < 0) {
                report_error("out of memory");
                rc = -1;
            }
        } else {
            log_info("skipping '%s' (not a regular file or directory)", sp);
        }
    }
    closedir(d);
    return rc;
}

/* Phase 2 */
static void *worker(void *arg)
{
    ctx_t *c = arg;
    job_t *j;

    while ((j = pop_job(c)) != NULL) {
        stats_t st = {0};
        int rc = copy_file(j->src, j->dst, c->opt, &st);

        if (rc == 0)
            printf("Copied: %s -> %s (%llu bytes)\n", j->src, j->dst, st.bytes);

        pthread_mutex_lock(&c->lock);
        if (rc == 0) {
            c->files_ok++;
            c->bytes += st.bytes;
        } else {
            c->files_failed++;
        }
        pthread_mutex_unlock(&c->lock);

        free(j->src);
        free(j->dst);
        free(j);
    }
    return NULL;
}

int copy_directory(const char *src, const char *dst,
                   const options_t *opt, stats_t *stats)
{
    ctx_t c;
    pthread_t tid[MAX_THREADS];
    dirnode_t *d, *next;
    int walk_rc, n, started = 0, i;
    double t0 = now_seconds();

    memset(&c, 0, sizeof(c));
    c.opt = opt;
    pthread_mutex_init(&c.lock, NULL);

    walk_rc = walk(&c, src, dst);

    /* never start more threads than there are files */
    n = opt->threads < 1 ? 1 : opt->threads;
    if (n > MAX_THREADS)
        n = MAX_THREADS;
    if ((size_t)n > c.queued)
        n = (int)c.queued;

    for (i = 0; i < n && n > 1; i++)
        if (pthread_create(&tid[started], NULL, worker, &c) == 0)
            started++;
    if (started == 0)
        worker(&c);                     /* single thread / fallback */
    for (i = 0; i < started; i++)
        pthread_join(tid[i], NULL);

    /* apply directory permissions once all files are in place */
    for (d = c.dirs; d != NULL; d = next) {
        next = d->next;
        if (chmod(d->path, d->mode & 07777) < 0)
            report_error("cannot set permissions on '%s': %s",
                         d->path, strerror(errno));
        free(d->path);
        free(d);
    }

    if (stats) {
        stats->bytes = c.bytes;
        stats->seconds = now_seconds() - t0;
        stats->files_ok = c.files_ok;
        stats->files_failed = c.files_failed;
    }
    pthread_mutex_destroy(&c.lock);
    return (walk_rc < 0 || c.files_failed > 0) ? -1 : 0;
}
