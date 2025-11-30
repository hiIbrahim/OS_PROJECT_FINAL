/* include/common.h */
#ifndef COMMON_H
#define COMMON_H

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <stdint.h>
#include <limits.h>
#include <libgen.h>
#include <stdarg.h>
#include <time.h>

/* Global variables */
extern char ROOT_PATH[PATH_MAX];
extern volatile pid_t fg_pgid;

/* Command history */
#define MAX_HISTORY 100
extern char *command_history[MAX_HISTORY];
extern int history_count;
extern int history_index;

/* Helper functions */
void dief(const char *fmt, ...);
int change_into_os_project(void);
int path_within_root(const char *resolved);

/* History functions */
void add_to_history(const char *cmd);
void print_history(void);
void save_history(void);
void load_history(void);

#endif /* COMMON_H */