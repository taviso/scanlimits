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
#include <glib.h>

#include "rlim.h"
#include "flags.h"

extern char **environ;

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
