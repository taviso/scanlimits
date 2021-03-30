#ifndef __FLAGS_H
#define __FLAGS_H

#include <sys/resource.h>

// See flags.c for documentation.
extern gint kMaxProcessTime;
extern guint kVerbosity;
extern struct rlimit kChildLimits[RLIMIT_NLIMITS];

#else
# warning flags.h included twice
#endif
