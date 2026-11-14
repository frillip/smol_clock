#ifndef NETWORK_H_STUB
#define NETWORK_H_STUB
// ntp.cpp must not set the host's clock.
#include <sys/time.h>
int fake_settimeofday(const struct timeval *, const void *);
#define settimeofday fake_settimeofday
#endif
