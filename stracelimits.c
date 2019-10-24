#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
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


// This is used to check if we recognise this error message from the rtld
// subprocess.
static uint32_t checksum(uint8_t *p, size_t len)
{
        uint32_t i;
        uint32_t crc = 0;

        for (crc = 0; len; len--) {
            crc ^= *p++;
            for (i = 0; i < 8; i++) {
                crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
            }
        }

        return crc;
}

static uint32_t set_all_limits(struct rlimit rlim[RLIMIT_NLIMITS])
{
    uint32_t i;

    for (i = 0; i < RLIMIT_NLIMITS; i++) {
        if (setrlimit(i, &rlim[i]) != 0) {
            warn("failed to setrlimit for %u", i);
        }
    }

    return 0;
}

static uint32_t get_all_limits(struct rlimit rlim[RLIMIT_NLIMITS])
{
    uint32_t i;

    for (i = 0; i < RLIMIT_NLIMITS; i++) {
        if (getrlimit(i, &rlim[i]) != 0) {
            warn("failed to getrlimit for %u", i);
        }
    }

    return 0;
}

char *num_to_str(char *buf, int n)
{
    sprintf(buf, "%#X", n);
    return buf;
}

static uint32_t spawn_process(struct rlimit rlim[RLIMIT_NLIMITS], char *output, size_t outputmax)
{
    int         pipefd[2];
    int         status;
    pid_t       child;
    char        *ptr;
    ssize_t     readlen   = 0;
    size_t      totalread = 0;
    char        buf[14][32];

    if (pipe(pipefd) != 0) {
        errx(1, "creating pipe for subprocess returned failure");
    }

    switch (child = fork()) {
        case 0:
            if (close(pipefd[0]) != 0) {
                errx(1, "unable to close read descriptor from pipe array");
            }

            if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
                errx(1, "failed to map stdout to pipe");
            }

            if (dup2(pipefd[1], STDERR_FILENO) == -1) {
                errx(1, "failed to map stderr to pipe");
            }

            //set_all_limits(rlim);
            if (execle("/usr/bin/sudo", "sudo", "/usr/bin/strace", "-eopen", "-utaviso", "./runlimit",
                                "RLIMIT_CPU",        num_to_str(buf[ 0], rlim[RLIMIT_CPU].rlim_cur),
                                "RLIMIT_FSIZE",      num_to_str(buf[ 1], rlim[RLIMIT_FSIZE].rlim_cur),
                                "RLIMIT_DATA",       num_to_str(buf[ 2], rlim[RLIMIT_DATA].rlim_cur),
                                "RLIMIT_STACK",      num_to_str(buf[ 3], rlim[RLIMIT_STACK].rlim_cur),
                                "RLIMIT_CORE",       num_to_str(buf[ 4], rlim[RLIMIT_CORE].rlim_cur),
                                "RLIMIT_RSS",        num_to_str(buf[ 5], rlim[RLIMIT_RSS].rlim_cur),
                                "RLIMIT_NOFILE",     num_to_str(buf[ 6], rlim[RLIMIT_NOFILE].rlim_cur),
                                "RLIMIT_AS",         num_to_str(buf[ 7], rlim[RLIMIT_AS].rlim_cur),
                                "RLIMIT_MEMLOCK",    num_to_str(buf[ 9], rlim[RLIMIT_MEMLOCK].rlim_cur),
                                "RLIMIT_LOCKS",      num_to_str(buf[ 10], rlim[RLIMIT_LOCKS].rlim_cur),
                                "RLIMIT_SIGPENDING", num_to_str(buf[ 11], rlim[RLIMIT_SIGPENDING].rlim_cur),
                                "RLIMIT_NICE",       num_to_str(buf[ 12], rlim[RLIMIT_NICE].rlim_cur),
                                "RLIMIT_RTPRIO",     num_to_str(buf[ 13], rlim[RLIMIT_RTPRIO].rlim_cur),
                             "pkexec", "poop",
                             NULL,
                             environ) == -1) {
                errx(1, "execve failed to launch requested binary");
            }

            // Not reached.
            errx(1, "execle returned unexpectedly");
        case -1:
            errx(1, "unable to create child process");
        default:
            if (close(pipefd[1]) != 0) {
                errx(1, "unable to close write descriptor from pipe array");
            }
    }

    // Initialize.
    memset(output, 0, outputmax);

    // Read all available output from child.
    do {
        readlen     = read(pipefd[0], output + totalread, outputmax - totalread);
        totalread   = totalread + readlen;
    } while (readlen > 0);

    if (readlen == -1) {
        errx(1, "there was an error reading child output from the pipe");
    }

    // Wait for the child to finish.
    if (waitpid(child, &status, 0) != child) {
        errx(1, "waitpid did not give us the process status we expected");
    }

    // Close the rest of the pipe.
    if (close(pipefd[0]) != 0) {
        errx(1, "unable to close read descriptor from pipe array");
    }

    if (WIFEXITED(status)) {
        totalread += snprintf(output + totalread,
                              outputmax - totalread,
                              "EXIT %d\n",
                              WEXITSTATUS(status),
                              checksum(output, totalread));
    } else if (WIFSIGNALED(status)) {
        totalread += snprintf(output + totalread,
                              outputmax - totalread,
                              "SIG %d\n",
                              WTERMSIG(status));
    } else {
        errx(1, "child process stopped for unexpected reason");
    }

    for (ptr = strstr(output, "/proc/"); ptr; ptr = strstr(ptr, "/proc/")) {
        for (ptr += 6; isdigit(*ptr); ptr++)
            *ptr = '?';
    }

    for (ptr = strstr(output, "si_addr="); ptr; ptr = strstr(ptr, "si_addr=")) {
        for (ptr += 8; isalnum(*ptr); ptr++)
            *ptr = '?';
    }

    //fprintf(stderr, "sample: %.10s...\n", output);

    return checksum(output, totalread);
}

bool check_known_output(uint32_t checksum)
{
    static uint32_t num_outputs;
    static uint32_t known_output[1024];
    int i;

    for (i = 0; i < num_outputs; i++) {
        if (known_output[i] == checksum)
            return true;
    }

    fprintf(stdout, "learnt new output checksum %#x\n", checksum);

    known_output[num_outputs++] = checksum;

    return false;
}

int main(int argc, char **argv)
{
    struct rlimit limits[RLIMIT_NLIMITS];
    char output[8192];
    int limit;
    int normal;
    int i;

    for (limit = 0; limit < RLIMIT_NLIMITS; limit++) {
        // Blacklist NPROC because it's difficult to handle generically.
        if (limit == RLIMIT_NPROC) {
            continue;
        }

        // Fetch our default limits.
        get_all_limits(limits);

        // Record the default output.
        normal = spawn_process(limits, output, sizeof output);

        fprintf(stderr, "process output for %s usually has checksum %#x\n", limit_to_str(limit), normal);

        // Now find a reasonable start point to reduce to.
        while (limits[limit].rlim_cur >>= 1) {
            if (spawn_process(limits, output, sizeof output) != normal) {
                fprintf(stderr, "found abnormal output for limit %s @%#x\n", limit_to_str(limit), limits[limit].rlim_cur);
                fprintf(stderr, "%s\n", output);
                limits[limit].rlim_cur <<= 1;
                break;
            }
        }

        while (limits[limit].rlim_cur > 0x1000) {
            limits[limit].rlim_cur -= 0x1000;

            if (spawn_process(limits, output, sizeof output) != normal) {
                normal = spawn_process(limits, output, sizeof output);

                limits[limit].rlim_cur += 0x1000;
                // Now reduce this limit until it reaches zero, and record each new
                // output.
                for (i = 0; i < 0x1000; i++) {
                    limits[limit].rlim_cur--;

                    if (check_known_output(spawn_process(limits, output, sizeof output)) == false) {
                        fprintf(stderr, "found new error message @limit %s->%#lx\n\t%s\n", limit_to_str(limit), limits[limit].rlim_cur, output);
                    }
                }
            }
        }

        // Now reduce this limit until it reaches zero, and record each new
        // output.
        while (limits[limit].rlim_cur--) {
            if (check_known_output(spawn_process(limits, output, sizeof output)) == false) {
                fprintf(stderr, "found new error message @limit %s->%#lx\n\t%s\n", limit_to_str(limit), limits[limit].rlim_cur, output);
            }
        }
    }

    return 0;
}
