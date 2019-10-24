#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>

extern char **environ;

static const char * limit_to_str(uint8_t limit)
{
    static char * limits[UINT8_MAX] = {
        [RLIMIT_CPU]        "RLIMIT_CPU",
        [RLIMIT_FSIZE]      "RLIMIT_FSIZE",
        [RLIMIT_DATA]       "RLIMIT_DATA",
        [RLIMIT_STACK]      "RLIMIT_STACK",
        [RLIMIT_CORE]       "RLIMIT_CORE",
        [RLIMIT_RSS]        "RLIMIT_RSS",
        [RLIMIT_NOFILE]     "RLIMIT_NOFILE",
        [RLIMIT_AS]         "RLIMIT_AS",
        [RLIMIT_NPROC]      "RLIMIT_NPROC",
        [RLIMIT_MEMLOCK]    "RLIMIT_MEMLOCK",
        [RLIMIT_LOCKS]      "RLIMIT_LOCKS",
        [RLIMIT_SIGPENDING] "RLIMIT_SIGPENDING",
        [RLIMIT_NICE]       "RLIMIT_NICE",
        [RLIMIT_RTPRIO]     "RLIMIT_RTPRIO",
        [RLIMIT_NLIMITS]    "RLIMIT_NLIMITS",
    };

    return limits[limit];
}

static const int str_to_limit(const char *limit)
{
    unsigned i;
    for (i = 0; i < RLIMIT_NLIMITS; i++) {
        if (limit_to_str(i) && strcmp(limit, limit_to_str(i)) == 0)
            return i;
    }

    return -1;
}

int main(int argc, char **argv)
{

    for (argv++; str_to_limit(*argv) >= 0; argv += 2) {
        struct rlimit rlimit = {
            .rlim_cur = strtoul(argv[1], NULL, 0),
            .rlim_max = strtoul(argv[1], NULL, 0),
        };

        if (setrlimit(str_to_limit(argv[0]), &rlimit) != 0) {
            fprintf(stderr, "setrlimit for %s failed, %m\n", argv[0]);
            return 1;
        }
    }

    execvpe(*argv, argv, environ);

    abort();
}
