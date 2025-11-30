/* include/shell.h */
#ifndef SHELL_H
#define SHELL_H

#include "common.h"

/* Command structure for pipeline */
typedef struct {
    char *argv[64];
    int argc;
    char *input_file;   // For input redirection
    char *output_file;  // For output redirection
    int append;         // Append mode for output
} command;

/* Shell builtin functions */
int is_shell_builtin(const char *cmd);
int do_shell_builtin(int argc, char **argv);

/* Command execution */
int parse_pipeline(char *line, command *cmds, int *num_cmds);
int execute_pipeline(command *cmds, int num_cmds);

/* Redirection helpers */
int setup_redirection(command *cmd);

#endif /* SHELL_H */