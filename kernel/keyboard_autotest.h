#ifndef KEYBOARD_AUTOTEST_H
#define KEYBOARD_AUTOTEST_H

/* Guest keyboard decode autotest via inject scancodes.
 * Emits [AUTOTEST]/[INF][autotest] markers on serial. */
int keyboard_autotest_run(void);

#endif
