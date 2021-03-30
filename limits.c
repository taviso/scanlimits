#include <stdio.h>
#include <stdbool.h>
#include <libgen.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/user.h>
#include <regex.h>
#include <glib.h>
#include <err.h>

#include "proc.h"
#include "flags.h"
#include "rlim.h"

static void print_usage(const char *name)
{
    const char *description =
        "Test how COMMAND reacts to reduced resource limits.\n\n"
        "\t-t TIMEOUT   Kill the process if it takes longer than this.\n"
        "\t-b FILTER    Load regex (one per line) to filter output.\n"
        "\t-o OUTPUT    Generate commands to see output in file.\n"
        "\t-i INFILE    Attach specified file to process stdin.\n"
        "\n"
        "Example:\n\n"
        "\tlimits -b filters.txt -o output -- /usr/bin/sudo";

    printf("%s [OPTIONS] [--] COMMAND [ARGS..]\n", name);
    puts(description);
    return;
}

// This routine reads a list of regex that should be removed from
// the output of programs being tested.
//
// The reason this is necessary is that some programs will add timestamps or
// pids to error messages and we have no way of knowing if this is a new error
// message, or an old one with a new timestamp.
//
// The list should be freed when no longer needed.
static GSList * parse_filterlist_file(const char *filename)
{
    GSList *list;
    gchar *file;
    gchar **patterns;

    // Read in the list of regex.
    if (g_file_get_contents(filename, &file, NULL, NULL) != true) {
        err(EXIT_FAILURE, "failed to open filter pattern file %s", filename);
        return NULL;
    }

    // Split that, one per line.
    patterns = g_strsplit(file, "\n", -1);

    // We return a linked list of compiled regex.
    list = NULL;

    // Attempt to compile each patten.
    for (guint i = 0; i < g_strv_length(patterns); i++) {
        GRegex *r;

        g_debug("attempting to parse pattern /%s/", patterns[i]);

        // Check if this is a comment.
        if (patterns[i][0] == '#' || patterns[i][0] == '\0') {
            continue;
        }

        // Not a comment, compile the regex.
        r = g_regex_new(patterns[i], G_REGEX_OPTIMIZE, 0, NULL);

        if (r == NULL) {
            errx(EXIT_FAILURE, "failed to compile expression %s", patterns[i]);
            continue;
        }

        // It compiled, append to the filter list.
        list = g_slist_append(list, r);
    }

    g_print("file %s contained %d valid filter patterns.\n",
            filename,
            g_slist_length(list));

cleanup:
    // cleanup.
    g_strfreev(patterns);
    g_free(file);
    return list;
}

// Combine the results of an execution into a string.
// Note: checksum is freed.
static gchar *create_output_key(int result, siginfo_t *info, gchar *checksum)
{
    gchar *hash = g_strdup_printf("%08X%08X%08X%s",
                                  result,
                                  info->si_status,
                                  info->si_code,
                                  checksum);
    g_free(checksum);
    return hash;
}

static void check_used_envvars(gchar **argv, gint timeout, GSList *filters)
{
    char **envp = g_get_environ();
    gchar *origsum;
    gchar *testsum;
    guint found;
    gint result;
    siginfo_t siginfo;

    g_print("testing what environment variables influence output...\n");

    // Record the default output.
    result = read_output_subprocess(argv,
                                    envp,
                                    NULL,
                                    &siginfo,
                                    &origsum,
                                    timeout,
                                    filters);

    // Calculate checksum.
    origsum = create_output_key(result, &siginfo, origsum);

    // Scan to see which variables make a difference.
    for (gint i = found = 0; i < g_strv_length(envp); i++) {
        gchar *saved = envp[i];

        envp[i]      = envp[0];
        envp[0]      = saved;

        g_debug("testing %s", saved);

        result = read_output_subprocess(argv,
                                        &envp[1],
                                        NULL,
                                        &siginfo,
                                        &testsum,
                                        timeout,
                                        filters);

        testsum = create_output_key(result, &siginfo, testsum);

        if (g_strcmp0(testsum, origsum) != 0) {
            gint len = strcspn(saved, "=");
            g_print("\t$%.*s\n", len, saved);
            found++;
        }

        g_free(testsum);
    }

    g_print("found %u variable(s) that change output\n", found);
    g_strfreev(envp);
    g_free(origsum);
    return;
}

static GHashTable *outputmap;
static gint timeout = 1;
static char **envp;
static char *stdinfile = "/dev/null";

int main(int argc, char **argv)
{
    struct rlimit limits[RLIMIT_NLIMITS];
    GSList *filters = NULL;
    gchar *command = NULL;
    FILE *logfile = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "i:t:ho:b:")) != -1) {
        switch (opt) {
            case 'h':
                print_usage(*argv);
                return 0;
            case 'b':
                filters = parse_filterlist_file(optarg);
                break;
            case 'o':
                logfile = fopen(optarg, "w");
                fprintf(logfile, "#!/bin/sh\n");
                fflush(logfile);
                break;
            case 't':
                timeout = g_ascii_strtoull(optarg, NULL, 0);
                break;
            case 'i':
                stdinfile = optarg;
                break;
            default:
                print_usage(*argv);
                return 1;
        }
    }

    if (optind >= argc) {
        errx(EXIT_FAILURE, "expected a command to test");
    }

    // Attach stdin to child processes.
    setup_proc_stdin(stdinfile);

    // Create a hashtable to store the known outputs.
    outputmap = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

    // Create a copy of the command for logfiles.
    command = g_strjoinv(" ", &argv[optind]);

    envp = g_get_environ();

    // Make any necessary changes to the childs environment.
    envp = g_environ_setenv(envp, "MALLOC_CHECK_", "2", false);

    check_used_envvars(&argv[optind], timeout, filters);

    // For each of the possible resource limits, we try to see if
    // it makes the program behave differently.
    //
    // If it does, we examine it more closely.
    for (gint limit = 0; limit < RLIMIT_NLIMITS; limit++) {
        gint result;
        gint distance;
        gchar *origsum;
        gchar *testsum;
        siginfo_t siginfo;

        g_debug("testing limit %d w/granularity %d",
                limit,
                get_limit_granularity(limit));

        // Skip any ignored limits.
        if (get_limit_granularity(limit) == 0) {
            continue;
        }

        g_print("searching %s...\n", limit_to_str(limit));

        // Initialize limits from our limits.
        init_limits_array(limits);

        // Record the default output.
        result = read_output_subprocess(&argv[optind],
                                        envp,
                                        limits,
                                        &siginfo,
                                        &origsum,
                                        timeout,
                                        filters);

        origsum = create_output_key(result, &siginfo, origsum);

        g_debug("default output key is %s", origsum);

        // Now find a reasonable start point by bisecting it until something
        // changes.
        while (limits[limit].rlim_cur >>= 1) {
            g_print("\t@%#020lx...", limits[limit].rlim_cur);

            result = read_output_subprocess(&argv[optind],
                                            envp,
                                            limits,
                                            &siginfo,
                                            &testsum,
                                            timeout,
                                            filters);

            testsum = create_output_key(result, &siginfo, testsum);

            // Check if this has changed.
            if (g_strcmp0(testsum, origsum) != 0) {
                // Put it back the way it was.
                limits[limit].rlim_cur <<= 1;

                // Put it one above to make sure we collect all errors.
                limits[limit].rlim_cur++;

                // Report that we found a new error.
                g_print("different\n");
                g_free(testsum);
                break;
            }

            g_free(testsum);
            g_print("same\r");
        }

        // Now we know where to start searching, so we can start reducing it by
        // the appropriate granularity. This is needed because for some limits
        // it's pointless testing every possible value, because only the
        // nearest page is actualy enforced.

        // Seed the normal output in the hashtable.
        g_hash_table_add(outputmap, origsum);

search:
        // Attempt to reduce the limit until we get a different result.
        for (distance = 0;
             limits[limit].rlim_cur >= get_limit_granularity(limit);
             limits[limit].rlim_cur -= get_limit_granularity(limit)) {
            gchar *checksum;

            // Sometimes things go really slowly, so increase granularity if
            // that's the case.
            limits[limit].rlim_cur -= MIN(get_limit_granularity(limit)
                                             * (distance / 32),
                                          limits[limit].rlim_cur);

            g_print("Testing %s = %#020lx...",
                    limit_to_str(limit),
                    limits[limit].rlim_cur);

            result = read_output_subprocess(&argv[optind],
                                            envp,
                                            limits,
                                            &siginfo,
                                            &checksum,
                                            timeout,
                                            filters);

            checksum = create_output_key(result, &siginfo, checksum);

            // Check if this key is known. Note that we don't need to free
            // checksum, the GDestroyNotify function in the GHashTable will
            // handle it.
            if (g_hash_table_add(outputmap, checksum)) {

                g_debug("checksum %s was not previously known", checksum);
                g_print("new\n");

                // If we have a logfile, try to make it a valid shellscript.
                if (logfile) {
                    fprintf(logfile, "%s/runlimit %s %#lx %s < %s\n",
                                     dirname(*argv),
                                     limit_to_str(limit),
                                     limits[limit].rlim_cur,
                                     command,
                                     stdinfile);
                    fflush(logfile);
                }

                // Reset how long it's been since we've seen a change.
                distance = 0;

                // If there are too many outputs, program might be printing
                // timestamps or something.
                if (g_hash_table_size(outputmap) == 128) {
                    warnx("There seems to be many different outputs.");
                    warnx("This is usually a sign you need to use filters.");
                }
            } else {
                g_print("\r");
            }

            // No need to continue.
            if (limits[limit].rlim_cur < get_limit_granularity(limit)) {
                break;
            }
        }

        // Make sure we don't finish on a \r
        g_print("\n");
    }

    if (!logfile && g_hash_table_size(outputmap)) {
        g_print("Hint: use -o output.sh to generate a test script\n");
    }

cleanup:

    g_hash_table_destroy(outputmap);

    if (logfile)
        fclose(logfile);

    if (filters)
        g_slist_free_full(filters, (GDestroyNotify) g_regex_unref);

    if (command)
        g_free(command);

    if (envp)
        g_strfreev(envp);

    return 0;
}
