#include <glib.h>
#include <stdbool.h>
#include <sys/resource.h>

#include "flags.h"

// If a process takes longer than this, we will send it SIGALRM.
gint kMaxProcessTime = 0;

// Increase for more debugging messages.
guint kVerbosity = 0;

// The rlimits we set in the child process, which can be configured via the
// commandline with --limit.
struct rlimit kChildLimits[RLIMIT_NLIMITS];

