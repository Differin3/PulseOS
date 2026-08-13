#ifndef FS_AUTOTEST_H
#define FS_AUTOTEST_H

/* Guest FS self-test. Returns 0 on success, -1 on failure.
 * Emits [AUTOTEST]/[INF][autotest] markers on serial. */
int fs_autotest_run(void);

#endif
