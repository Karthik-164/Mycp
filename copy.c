/* copy.c - single file copy: Levels 1-5, verification, resume,
 *          plus the file-info display and progress bar helpers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pwd.h>
#include <sys/stat.h>

#include "mycp.h"

#define BAR_WIDTH 30

/* ---------- Level 3: file information display ---------- */
static void print_file_info(const char *label, const char *path,
                            const struct stat *st)
{
    char size[32], perm[11], mtime[32];
    struct tm tm;
    struct passwd *pw = getpwuid(st->st_uid);
    mode_t m = st->st_mode;

    format_size((unsigned long long)st->st_size, size, sizeof(size));
    snprintf(perm, sizeof(perm), "%c%c%c%c%c%c%c%c%c%c",
             S_ISDIR(m) ? 'd' : S_ISREG(m) ? '-' : '?',
             (m & S_IRUSR) ? 'r' : '-', (m & S_IWUSR) ? 'w' : '-',
             (m & S_IXUSR) ? 'x' : '-', (m & S_IRGRP) ? 'r' : '-',
             (m & S_IWGRP) ? 'w' : '-', (m & S_IXGRP) ? 'x' : '-',
             (m & S_IROTH) ? 'r' : '-', (m & S_IWOTH) ? 'w' : '-',
             (m & S_IXOTH) ? 'x' : '-');
    localtime_r(&st->st_mtime, &tm);
    strftime(mtime, sizeof(mtime), "%Y-%m-%d %H:%M:%S", &tm);

    printf("---- %s file info ----\n", label);
    printf("  Path        : %s\n", path);
    printf("  Size        : %lld bytes (%s)\n", (long long)st->st_size, size);
    printf("  Permissions : %s (%04o)\n", perm, (unsigned)(m & 07777));
    printf("  Owner       : %s (uid %u)\n", pw ? pw->pw_name : "?", (unsigned)st->st_uid);
    printf("  Inode       : %llu\n", (unsigned long long)st->st_ino);
    printf("  Hard links  : %lu\n", (unsigned long)st->st_nlink);
    printf("  Modified    : %s\n", mtime);
}

/* ---------- Level 4: progress bar (single-threaded use only) ---------- */
static int g_last_pct = -1;

static void progress_update(unsigned long long done, unsigned long long total)
{
    char bar[BAR_WIDTH + 1], d[32], t[32];
    int pct, filled, i;

    if (total == 0)
        return;
    pct = (int)((double)done * 100.0 / (double)total);
    if (pct > 100)
        pct = 100;
    if (pct == g_last_pct)
        return;
    g_last_pct = pct;

    filled = pct * BAR_WIDTH / 100;
    for (i = 0; i < BAR_WIDTH; i++)
        bar[i] = (i < filled) ? '#' : '.';
    bar[BAR_WIDTH] = '\0';

    format_size(done, d, sizeof(d));
    format_size(total, t, sizeof(t));
    printf("\r[%s] %3d%%  %s / %s", bar, pct, d, t);
    fflush(stdout);
}

static void progress_finish(void)
{
    if (g_last_pct >= 0) {
        printf("\n");
        fflush(stdout);
    }
    g_last_pct = -1;
}

/* ---------- copy verification: byte-by-byte compare ---------- */
/* Returns 0 identical, 1 different, -1 error. */
static int verify_files(const char *a, const char *b, size_t bufsize)
{
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY), result = 0;
    char *ba = malloc(bufsize), *bb = malloc(bufsize);

    if (fa < 0 || fb < 0 || !ba || !bb) {
        report_error("verify: cannot open files or allocate memory");
        result = -1;
    }
    while (result == 0) {
        ssize_t na = read_full(fa, ba, bufsize);
        ssize_t nb = read_full(fb, bb, bufsize);

        if (na < 0 || nb < 0) {
            report_error("verify: read error: %s", strerror(errno));
            result = -1;
        } else if (na != nb || memcmp(ba, bb, (size_t)na) != 0) {
            result = 1;
        } else if (na == 0) {
            break;
        }
    }
    free(ba);
    free(bb);
    if (fa >= 0) close(fa);
    if (fb >= 0) close(fb);
    return result;
}

/* ---------- the copy itself ---------- */
int copy_file(const char *src, const char *dst,
              const options_t *opt, stats_t *stats)
{
    struct stat sst, dstat;
    int in = -1, out = -1, ret = -1, have_dst, rc, v;
    char *buf = NULL;
    unsigned long long offset = 0, copied = 0, total;
    double t0 = now_seconds();
    ssize_t n;

    /* open and inspect the source */
    in = open(src, O_RDONLY);
    if (in < 0) {
        report_error("cannot open '%s': %s", src, strerror(errno));
        goto cleanup;
    }
    if (fstat(in, &sst) < 0) {
        report_error("cannot stat '%s': %s", src, strerror(errno));
        goto cleanup;
    }
    if (!S_ISREG(sst.st_mode)) {
        report_error("'%s' is not a regular file", src);
        goto cleanup;
    }
    total = (unsigned long long)sst.st_size;

    /* refuse to copy a file onto itself */
    have_dst = (stat(dst, &dstat) == 0);
    if (have_dst && dstat.st_dev == sst.st_dev && dstat.st_ino == sst.st_ino) {
        report_error("'%s' and '%s' are the same file", src, dst);
        goto cleanup;
    }

    /* resume: continue from the size of a partial destination */
    if (opt->resume && have_dst && S_ISREG(dstat.st_mode) &&
        (unsigned long long)dstat.st_size <= total)
        offset = (unsigned long long)dstat.st_size;

    if (opt->show_info)
        print_file_info("Source", src, &sst);

    /* open the destination */
    out = open(dst, O_WRONLY | O_CREAT | (offset ? 0 : O_TRUNC), 0644);
    if (out < 0) {
        report_error("cannot create '%s': %s", dst, strerror(errno));
        goto cleanup;
    }
    if (offset > 0) {
        if (lseek(in, (off_t)offset, SEEK_SET) < 0 ||
            lseek(out, (off_t)offset, SEEK_SET) < 0) {
            report_error("lseek failed: %s", strerror(errno));
            goto cleanup;
        }
        printf("Resuming '%s' from byte %llu of %llu\n", dst, offset, total);
        log_info("resuming %s from byte %llu", dst, offset);
    }

    buf = malloc(opt->buf_size);
    if (!buf) {
        report_error("out of memory (buffer %zu bytes)", opt->buf_size);
        goto cleanup;
    }

    /* main loop: read a block, write the whole block */
    g_last_pct = -1;
    for (;;) {
        n = read(in, buf, opt->buf_size);
        if (n == 0)
            break;                                  /* end of file */
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            if (opt->progress) progress_finish();
            report_error("read error on '%s': %s", src, strerror(errno));
            goto cleanup;
        }
        if (write_all(out, buf, (size_t)n) < 0) {
            if (opt->progress) progress_finish();
            report_error("write error on '%s': %s", dst, strerror(errno));
            goto cleanup;
        }
        copied += (unsigned long long)n;
        if (opt->progress)
            progress_update(offset + copied, total);
    }
    if (opt->progress)
        progress_finish();

    /* Level 5: preserve permissions */
    if (opt->preserve && fchmod(out, sst.st_mode & 07777) < 0) {
        report_error("cannot set permissions on '%s': %s", dst, strerror(errno));
        goto cleanup;
    }
    rc = close(out);
    out = -1;
    if (rc < 0) {
        report_error("close error on '%s': %s", dst, strerror(errno));
        goto cleanup;
    }

    if (opt->show_info && stat(dst, &dstat) == 0)
        print_file_info("Destination", dst, &dstat);

    if (opt->verify) {
        v = verify_files(src, dst, opt->buf_size);
        if (v == 1)
            report_error("verification FAILED: '%s' differs from '%s'", dst, src);
        if (v != 0)
            goto cleanup;
        printf("Verified: '%s' matches '%s'\n", dst, src);
    }

    log_info("copied %s -> %s (%llu bytes)", src, dst, copied);
    ret = 0;

cleanup:
    free(buf);
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    if (stats) {
        stats->bytes = copied;
        stats->seconds = now_seconds() - t0;
    }
    return ret;
}
