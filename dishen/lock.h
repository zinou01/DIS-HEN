/* lock.h - single-instance pid lockfile. */
#ifndef DISHEN_LOCK_H
#define DISHEN_LOCK_H
#define LOCK_PATH "/data/Dis-HEN/daemon.lock"
/* 0 acquired, 1 live peer holds it (stand down), -1 error/retry. */
int lock_acquire(void);
void lock_release(void);
#endif
