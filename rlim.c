#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/user.h>
#include <sys/resource.h>
#include <glib.h>

#include "flags.h"
#include "rlim.h"

// Parse and setup rlimits for child processes.
const gchar * limit_to_str(uint8_t limit)
{
    static const gchar * limits[UINT8_MAX] = {
        [RLIMIT_CPU]        = "RLIMIT_CPU",
        [RLIMIT_FSIZE]      = "RLIMIT_FSIZE",
        [RLIMIT_DATA]       = "RLIMIT_DATA",
        [RLIMIT_STACK]      = "RLIMIT_STACK",
        [RLIMIT_CORE]       = "RLIMIT_CORE",
        [RLIMIT_RSS]        = "RLIMIT_RSS",
        [RLIMIT_NOFILE]     = "RLIMIT_NOFILE",
        [RLIMIT_NPROC]      = "RLIMIT_NPROC",
        [RLIMIT_MEMLOCK]    = "RLIMIT_MEMLOCK",
        [RLIMIT_AS]         = "RLIMIT_AS",
        [RLIMIT_LOCKS]      = "RLIMIT_LOCKS",
        [RLIMIT_SIGPENDING] = "RLIMIT_SIGPENDING",
        [RLIMIT_MSGQUEUE]   = "RLIMIT_MSGQUEUE",
        [RLIMIT_NICE]       = "RLIMIT_NICE",
        [RLIMIT_RTPRIO]     = "RLIMIT_RTPRIO",
        [RLIMIT_RTTIME]     = "RLIMIT_RTTIME",
    };

    return limits[limit];
}

gint get_limit_granularity(uint8_t limit)
{
    static const gint limits[UINT8_MAX] = {
        [RLIMIT_CPU]        = 1,
        [RLIMIT_FSIZE]      = 1,
        [RLIMIT_DATA]       = PAGE_SIZE,
        [RLIMIT_STACK]      = PAGE_SIZE,
        [RLIMIT_CORE]       = 0,
        [RLIMIT_RSS]        = PAGE_SIZE,
        [RLIMIT_NOFILE]     = 1,
        [RLIMIT_NPROC]      = 0,
        [RLIMIT_MEMLOCK]    = PAGE_SIZE,
        [RLIMIT_AS]         = PAGE_SIZE,
        [RLIMIT_LOCKS]      = 1,
        [RLIMIT_SIGPENDING] = 1,
        [RLIMIT_MSGQUEUE]   = 1,
        [RLIMIT_NICE]       = 1,
        [RLIMIT_RTPRIO]     = 1,
        [RLIMIT_RTTIME]     = 1,
    };

    return limits[limit];
}

gint str_to_limit(const gchar *limit)
{
    guint i;
    for (i = 0; i < RLIMIT_NLIMITS; i++) {
        if (limit_to_str(i) && strcmp(limit, limit_to_str(i)) == 0)
            return i;
    }

    return -1;
}

gboolean decode_proc_limit(const gchar *option_name,
                           const gchar *value,
                           gpointer data,
                           GError **error)
{
    gint resource;
    gint limit;
    gchar **param;

    g_assert_cmpstr(option_name, ==, "--limit");
    g_assert_nonnull(value);

    // value will be of them form RLIMIT_FOO=12345.
    param = g_strsplit(value, "=", 2);

    // Check we got two parameters.
    if (param[0] == NULL || param[1] == NULL) {
        g_warning("You passed the string %s to %s, but that is not a valid limit specification",
                  value,
                  option_name);
        g_warning("See setrlimit(3) manual for a full list, for example, --limit RLIMIT_CPU=120");
        g_strfreev(param);
        return false;
    }

    // Decode those two options.
    resource    = str_to_limit(param[0]);
    limit       = g_ascii_strtoll(param[1], NULL, 0);

    if (resource == -1) {
        g_warning("You passed the string %s to %s, but `%s` is not recognized as a limit name",
                  value,
                  option_name,
                  *param);
        g_warning("See setrlimit(3) manual for a full list, for example, --limit RLIMIT_CPU=120");
        g_strfreev(param);
        return false;
    }

    kChildLimits[resource].rlim_cur = limit;
    kChildLimits[resource].rlim_max = limit;

    g_strfreev(param);
    return true;
}

void init_limits_array(struct rlimit rlim[RLIMIT_NLIMITS])
{
    g_debug("configuring default rlimits for child process");

    for (gint i = 0; i < RLIMIT_NLIMITS; i++) {
        if (getrlimit(i, &rlim[i]) != 0) {
            g_warning("failed to getrlimit for %u, %s", i, strerror(errno));
        }

        g_debug("Configured rlimit %s => { %lu, %lu }",
                limit_to_str(i),
                rlim[i].rlim_cur,
                rlim[i].rlim_max);
    }

    // OK, but lets set some sane defaults.
    kChildLimits[RLIMIT_CORE].rlim_cur = 0;
    kChildLimits[RLIMIT_CORE].rlim_max = 0;
}

