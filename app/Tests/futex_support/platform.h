#pragma once
#if !defined(__APPLE__)
#include <pthread.h>
#include <time.h>
// Linux-only test bridge for Darwin's relative monotonic condition wait. The
// production Apple path and its timeout calculation are compiled unchanged.
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *condition,
  pthread_mutex_t *mutex, const struct timespec *remaining) {
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += remaining->tv_sec;
  deadline.tv_nsec += remaining->tv_nsec;
  if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
  return pthread_cond_clockwait(condition, mutex, CLOCK_MONOTONIC, &deadline);
}
#endif
