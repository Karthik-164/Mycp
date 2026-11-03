/* main.c - argument parsing, program flow, summary and benchmark */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

#include "mycp.h"

static void usage(FILE *f, const char *prog)
{
    fprintf(f,
        "Usage: %s [options] <source> <destination>\n"
        "\n"
        "Options:\n"
        "  -b SIZE   buffer size in bytes, e.g. 4096, 64K, 1M   (default 4096)\n"
        "  -i        show file information (source and destination)\n"
        "  -p        show progress bar\n"
        "  -m        preserve file permissions\n"
        "  -r        recursive directory copy\n"
        "  -j N      parallel copy with N threads (with -r)\n"
        "  -v        verify the copy (byte-by-byte comparison)\n"
        "  -c        continue/resume an interrupted copy\n"
        "  -B        benchmark: copy with several buffer sizes and compare\n"
        "  -l FILE   write a log to FILE\n"
        "  -h        show this help\n",
        prog);
}

static void print_summary(const stats_t *s, int is_dir)
{
    char size[32];
    double mbps = s->seconds > 0 ? ((double)s->bytes / 1048576.0) / s->seconds : 0.0;

    format_size(s->bytes, size, sizeof(size));
    printf("\n---- Summary ----\n");
    if (is_dir) {
        printf("Files copied : %lu\n", s->files_ok);
        printf("Files failed : %lu\n", s->files_failed);
    }
    printf("Bytes copied : %llu (%s)\n", s->bytes, size);
    printf("Time taken   : %.4f s\n", s->seconds);
    printf("Speed        : %.2f MB/s\n", mbps);
}

/* Copy the file with several buffer sizes and print a comparison table. */
static int run_benchmark(const char *src, const char *dst, const options_t *opt)
{
    static const size_t sizes[] = { 512, 1024, 4096, 8192, 16384, 65536, 262144, 1048576 };
    const size_t count = sizeof(sizes) / sizeof(sizes[0]);
    char tmp[PATH_MAX], label[32], total[32];
    double secs[8];
    size_t i, best = 0;
    struct stat st;

    if (snprintf(tmp, sizeof(tmp), "%s.bench.tmp", dst) >= (int)sizeof(tmp) ||
        stat(src, &st) < 0) {
        report_error("benchmark: bad source or destination path");
        return -1;
    }
    format_size((unsigned long long)st.st_size, total, sizeof(total));
    printf("Benchmark: copying '%s' (%s) with different buffer sizes\n", src, total);
    printf("%-12s %-12s %s\n", "Buffer", "Time (s)", "Speed (MB/s)");
    printf("------------ ------------ ------------\n");

    for (i = 0; i < count; i++) {
        options_t o = *opt;
        stats_t s = {0};
        double mbps;

        o.buf_size = sizes[i];
        o.progress = o.show_info = o.verify = o.resume = 0;
        if (copy_file(src, tmp, &o, &s) < 0) {
            unlink(tmp);
            return -1;
        }
        unlink(tmp);

        secs[i] = s.seconds;
        mbps = s.seconds > 0 ? ((double)s.bytes / 1048576.0) / s.seconds : 0.0;
        format_size(sizes[i], label, sizeof(label));
        printf("%-12s %-12.4f %.2f\n", label, s.seconds, mbps);
        log_info("benchmark buffer=%zu time=%.4fs speed=%.2fMB/s", sizes[i], s.seconds, mbps);
        if (secs[i] < secs[best])
            best = i;
    }
    format_size(sizes[best], label, sizeof(label));
    printf("\nFastest buffer size: %s\n", label);
    return 0;
}

static int run(const options_t *opt, const char *src, const char *dst)
{
    struct stat sst, dt;
    stats_t st = {0};
    char target[PATH_MAX];
    const char *final_dst = dst;
    int rc;

    if (stat(src, &sst) < 0) {
        report_error("cannot stat '%s': %s", src, strerror(errno));
        return -1;
    }

    if (opt->benchmark) {
        if (!S_ISREG(sst.st_mode)) {
            report_error("benchmark needs a regular file as source");
            return -1;
        }
        return run_benchmark(src, dst, opt);
    }

    if (S_ISDIR(sst.st_mode)) {                 /* directory source */
        options_t o = *opt;

        if (!opt->recursive) {
            report_error("'%s' is a directory (use -r to copy directories)", src);
            return -1;
        }
        if (o.threads > 1 && (o.progress || o.show_info)) {
            printf("Note: progress bar and file info are disabled with multiple threads\n");
            o.progress = o.show_info = 0;
        }
        rc = copy_directory(src, dst, &o, &st);
        print_summary(&st, 1);
        return rc;
    }

    /* single file; if dst is an existing directory, copy into it */
    if (stat(dst, &dt) == 0 && S_ISDIR(dt.st_mode)) {
        if (join_path(target, sizeof(target), dst, base_name(src)) < 0) {
            report_error("destination path too long");
            return -1;
        }
        final_dst = target;
    }
    rc = copy_file(src, final_dst, opt, &st);
    if (rc == 0) {
        printf("Copied '%s' -> '%s'\n", src, final_dst);
        print_summary(&st, 0);
    }
    return rc;
}

int main(int argc, char *argv[])
{
    options_t opt;
    int c, rc;

    memset(&opt, 0, sizeof(opt));
    opt.buf_size = DEFAULT_BUF_SIZE;
    opt.threads = 1;

    while ((c = getopt(argc, argv, "b:ipmrj:vcBl:h")) != -1) {
        switch (c) {
        case 'b':
            if (parse_size(optarg, &opt.buf_size) < 0) {
                fprintf(stderr, "mycp: invalid buffer size '%s' "
                        "(use 1 byte to 256M, e.g. 4096, 64K, 1M)\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'i': opt.show_info = 1; break;
        case 'p': opt.progress = 1;  break;
        case 'm': opt.preserve = 1;  break;
        case 'r': opt.recursive = 1; break;
        case 'j': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end != '\0' || v < 1 || v > MAX_THREADS) {
                fprintf(stderr, "mycp: invalid thread count '%s' (1-%d)\n",
                        optarg, MAX_THREADS);
                return EXIT_FAILURE;
            }
            opt.threads = (int)v;
            break;
        }
        case 'v': opt.verify = 1;    break;
        case 'c': opt.resume = 1;    break;
        case 'B': opt.benchmark = 1; break;
        case 'l': opt.log_path = optarg; break;
        case 'h':
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (argc - optind != 2) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    if (opt.log_path && log_open(opt.log_path) < 0)
        return EXIT_FAILURE;

    log_info("mycp started: '%s' -> '%s' (buffer=%zu)",
             argv[optind], argv[optind + 1], opt.buf_size);
    rc = run(&opt, argv[optind], argv[optind + 1]);
    log_info("mycp finished: %s", rc == 0 ? "success" : "failure");
    log_close();
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
