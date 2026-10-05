/* Every thread of ours names itself as it starts, so that the task manager (taskmgr.c, "hassmic threads" in Home
 * Assistant), scripts/top.sh and top -H tell them apart; unnamed they all show the process's "hassmic".  The kernel keeps
 * 15 characters and cuts the rest.  Not the main thread: its name is the process's, which pidof matches (main.sh). */
#ifndef THREADNAME_H
#define THREADNAME_H
#include <sys/prctl.h>
static inline void thread_name(const char *name) { prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0); }
#endif
