#ifndef __RLIM_H
#define __RLIM_H

const gchar * limit_to_str(uint8_t limit);
gint get_limit_granularity(uint8_t limit);
void init_limits_array(struct rlimit rlim[RLIMIT_NLIMITS]);
gint str_to_limit(const gchar *limit);


#endif
