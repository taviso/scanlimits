#ifndef __PROC_H
#define __PROC_H

void setup_proc_stdin(const char *filename);

gint read_output_subprocess(char **argv,
                            char **envp,
                            struct rlimit *limits,
                            siginfo_t *info,
                            gchar **checksum,
                            int timeout,
                            GSList *filter);


#else
# warning proc.h included twice
#endif
