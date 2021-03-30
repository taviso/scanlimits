#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/personality.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>

#include "proc.h"

extern char **environ;

// Setup and execute child processes.

// This routine is called in the child process before the execve(), so it's
// useful for configuring limits and file descriptors, prctl and so on. Note
// that intefering with the heap will most likely deadlock the process (even
// calling g_message or whatever, which can cause a malloc()).
static void configure_child_limits(gpointer userdata)
{
    struct rlimit *limits = userdata;

    // Make sure we create a new pgrp so that we can kill all subprocesses.
    setpgid(0, 0);

    // Try to cleanup if we get killed.
    prctl(PR_SET_PDEATHSIG, SIGKILL);

    // If no limits, we dont have to do anything.
    if (userdata == NULL)
        return;

    // Some of these may fail, not sure what to do.
    for (gint i = 0; i < RLIMIT_NLIMITS; i++) {

        // Don't let sneaky programs (e.g. sudo) raise limits.
        limits[i].rlim_max = MIN(limits[i].rlim_cur, limits[i].rlim_max);
        limits[i].rlim_cur = MIN(limits[i].rlim_cur, limits[i].rlim_max);

        if (setrlimit(i, &limits[i]) == -1) {
            err(EXIT_FAILURE, "setrlimit for %u failed", i);
        }
    }

    return;
}

static gint infd = -1;

void setup_proc_stdin(const char *filename)
{
    if (infd != -1)
        close(infd);

    infd = open(filename, O_RDONLY);

    if (infd == -1) {
        err(EXIT_FAILURE, "failed to open %s", filename);
    }
}

void __attribute__((constructor)) setup_file_descriptors()
{
    // Choose a sane default.
    setup_proc_stdin("/dev/null");
}

void __attribute__((destructor)) close_file_descriptors()
{
    close(infd);
}

gint read_output_subprocess(char **argv,
                            char **envp,
                            struct rlimit *limits,
                            siginfo_t *info,
                            gchar **hash,
                            int timeout,
                            GSList *filter)
{
    GChecksum *outhash = NULL;
    GChecksum *errhash = NULL;
    GError *error = NULL;
    GTimer *timer = NULL;
    pid_t childpid;
    gchar outbuf[1024];
    gchar errbuf[1024];
    gint outfd[2];
    gint errfd[2];
    gint flags;
    gsize totalread;

    // I want to reap the child myself.
    flags = G_SPAWN_DO_NOT_REAP_CHILD;

    // Initialize output.
    *hash = NULL;

    memset(info, 0, sizeof *info);

    // Make some pipes so we can collect stdout/stderr.
    if (pipe(outfd) != 0 || pipe(errfd) != 0) {
        err(EXIT_FAILURE, "failed to create pipes for child process");
    }

    // Create child process.
    switch (childpid = fork()) {
        case 0:
            // Close the read side of these pipes.
            close(outfd[0]);
            close(errfd[0]);

            // Now move stdin/stdout/stderr onto the pipes.
            dup2(outfd[1], STDOUT_FILENO);
            dup2(errfd[1], STDERR_FILENO);
            dup2(infd, STDIN_FILENO);

            // Reset infd, I don't care if it fails.
            lseek(infd, 0, SEEK_SET);

            // Close any straggling file descriptors.
            for (int i = 3; i < 128; i++)
                close(i);

            // Setup the limits we're supposed to be using
            configure_child_limits(limits);

            // Execute test
            execvpe(*argv, argv, envp);

            err(EXIT_FAILURE, "execve failed unexpectedly");
        case -1:
            err(EXIT_FAILURE, "failed to create child process");
        default:
            // Close the write side of the pipes.
            close(outfd[1]);
            close(errfd[1]);
    }

    // Mark my descriptors non-blocking.
    if (fcntl(outfd[0], F_SETFL, O_NONBLOCK) < 0)
        g_error("failed to set descriptor to nonblocking");

    if (fcntl(errfd[0], F_SETFL, O_NONBLOCK) < 0)
        g_error("failed to set descriptor to nonblocking");

    // Setup a checksum to hash the output.
    outhash = g_checksum_new(G_CHECKSUM_MD5);
    errhash = g_checksum_new(G_CHECKSUM_MD5);

    // Reset how much data we've read, this is only used
    // if the caller wants a sample of output.
    totalread = 0;

    // Start timing how long this takes.
    timer = g_timer_new();

    g_assert_cmpint(childpid, >, 1);

    // Now we keep reading output from the child until it completes,
    // or we reach the timout.
    while (true) {
        ssize_t oreadlen;
        ssize_t ereadlen;
        int oerrno;
        int eerrno;

        // If this is taking too long, kill it.
        if (g_timer_elapsed(timer, NULL) > timeout) {
            kill(-childpid, SIGKILL);
        }

        oerrno = eerrno = errno = 0;

        oreadlen = read(outfd[0], outbuf, sizeof outbuf);
        oerrno   = errno;
        errno    = 0;
        ereadlen = read(errfd[0], errbuf, sizeof errbuf);
        eerrno   = errno;

        if (oreadlen > 0) {
            gchar *str = g_strndup(outbuf, oreadlen);

            //g_debug("stdout: %s", str);

            // Apply any requested transformations.
            for (GSList *p = filter; p; p = p->next) {
                gchar *newstr = g_regex_replace_literal(p->data,
                                                        str,
                                                        -1,
                                                        0,
                                                        "",
                                                        0,
                                                        NULL);

                if (g_regex_match(p->data, str, 0, NULL)) {
                    g_debug("pattern matched => %s => %s", str, newstr);
                }

                // Free original string.
                g_free(str);

                // Use the replacement string.
                str = newstr;
            }

            // Update the checksum
            g_checksum_update(outhash, (gpointer)(str), strlen(str));

            // Free string
            g_free(str);
        }

        if (ereadlen > 0) {
            gchar *str = g_strndup(errbuf, ereadlen);

            g_debug("stderr (%ld): %s", ereadlen, str);

            // Apply any requested transformations.
            for (GSList *p = filter; p; p = p->next) {
                gchar *newstr = g_regex_replace_literal(p->data,
                                                        str,
                                                        -1,
                                                        0,
                                                        "",
                                                        0,
                                                        NULL);

                if (g_regex_match(p->data, str, 0, NULL)) {
                    g_debug("pattern matched => %s => %s", str, newstr);
                }

                // Free original string.
                g_free(str);

                // Use the replacement string.
                str = newstr;
            }

            // Update the checksum
            g_checksum_update(errhash, (gpointer)(str), strlen(str));

            // Free string
            g_free(str);
        }

        if (oreadlen < 0 && !(oerrno == EAGAIN || oerrno == EINTR)) {
            g_critical("stdout read gave unexpected error %d", oerrno);
            g_assert_not_reached();
        }

        if (ereadlen < 0 && !(eerrno == EAGAIN || eerrno == EINTR)) {
            g_critical("stderr read gave unexpected error %d", eerrno);
            g_assert_not_reached();
        }

        // The pipe must be closed.
        if (oreadlen == 0 && ereadlen == 0)
            break;

        // The pipe must be blocking.
        g_usleep(G_USEC_PER_SEC / 100);

    }

  childwait:
    // The data has been written to the child process, now we wait for it to
    // complete.
    if (waitid(P_PID, childpid, info, WEXITED | WNOHANG) != 0) {
        // On macOS, waitid can fail with EINTR, I don't think this can happen
        // on Linux but it doesn't hurt to handle it.
        if (errno != EINTR) {
            g_error("waitid for child %d failed, %s",
                    childpid,
                    strerror(errno));
        }

        // Continue waiting...
        goto childwait;
    }

    // The call would have blocked, check if there was a timeout.
    if (info->si_pid == 0) {
        if (g_timer_elapsed(timer, NULL) > timeout)
            kill(-childpid, SIGKILL);
        goto childwait;
    }

    g_assert_cmpint(info->si_pid, ==, childpid);

    switch (info->si_code) {
        case CLD_EXITED:
            //g_debug("child %d exited with code %d",
            //        info->si_pid,
            //        info->si_status);
            break;
        case CLD_DUMPED:
            g_debug("child %d dumped core, adjust limits?", info->si_pid);
            // fallthrough
        case CLD_KILLED:
            g_debug("child %d was killed by signal %s",
                    info->si_pid,
                    strsignal(info->si_status));
            break;
        case CLD_STOPPED:
        case CLD_TRAPPED:
        default:
            g_assert_not_reached();
    }

    // Copy the output checksum to caller.
    *hash = g_strjoin("-", g_checksum_get_string(outhash),
                           g_checksum_get_string(errhash),
                           NULL);

    // Clean up.
    g_clear_error(&error);
    g_timer_destroy(timer);
    g_checksum_free(outhash);
    g_checksum_free(errhash);
    g_close(errfd[0], NULL);
    g_close(outfd[0], NULL);
    return 0;
}
