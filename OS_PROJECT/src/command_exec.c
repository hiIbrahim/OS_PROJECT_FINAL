#include "../include/shell.h"
#include "../include/fat_fs.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Trim helpers */
static char *ltrim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}
static void rtrim_inplace(char *s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n-1])) s[--n] = '\0';
}

/* Parse pipeline: very simple tokenizer (doesn't handle quotes) */
int parse_pipeline(char *line, command *cmds, int *num_cmds) {
    *num_cmds = 0;
    if (!line) return 0;

    char *saveptr = NULL;
    char *cmd_str = strtok_r(line, "|", &saveptr);

    while (cmd_str && *num_cmds < 10) {
        char *s = ltrim(cmd_str);
        rtrim_inplace(s);

        cmds[*num_cmds].argc = 0;
        cmds[*num_cmds].input_file = NULL;
        cmds[*num_cmds].output_file = NULL;
        cmds[*num_cmds].append = 0;

        /* detect redirections (we only support simple forms: < infile, > outfile, >> outfile) */
        char *in_pos = strchr(s, '<');
        char *append_pos = strstr(s, ">>");
        char *out_pos = strchr(s, '>');

        /* handle input redir */
        if (in_pos) {
            *in_pos = '\0';
            char *arg = in_pos + 1;
            while (*arg && isspace((unsigned char)*arg)) arg++;
            char *end = arg;
            while (*end && !isspace((unsigned char)*end)) end++;
            if (*end) { *end = '\0'; }
            cmds[*num_cmds].input_file = strdup(arg);
            rtrim_inplace(s);
        }

        /* handle output redir (>> has precedence) */
        if (append_pos) {
            *append_pos = '\0';
            char *arg = append_pos + 2;
            while (*arg && isspace((unsigned char)*arg)) arg++;
            char *end = arg;
            while (*end && !isspace((unsigned char)*end)) end++;
            if (*end) { *end = '\0'; }
            cmds[*num_cmds].output_file = strdup(arg);
            cmds[*num_cmds].append = 1;
            rtrim_inplace(s);
        } else if (out_pos) {
            *out_pos = '\0';
            char *arg = out_pos + 1;
            while (*arg && isspace((unsigned char)*arg)) arg++;
            char *end = arg;
            while (*end && !isspace((unsigned char)*end)) end++;
            if (*end) { *end = '\0'; }
            cmds[*num_cmds].output_file = strdup(arg);
            cmds[*num_cmds].append = 0;
            rtrim_inplace(s);
        }

        /* split s into argv tokens */
        char *tk;
        char *tok_save = NULL;
        tk = strtok_r(s, " \t", &tok_save);
        int a = 0;
        while (tk && a < 63) {
            cmds[*num_cmds].argv[a++] = strdup(tk);
            tk = strtok_r(NULL, " \t", &tok_save);
        }
        cmds[*num_cmds].argv[a] = NULL;
        cmds[*num_cmds].argc = a;

        if (cmds[*num_cmds].argc > 0) {
            (*num_cmds)++;
        } else {
            /* free any allocated redir strings if empty command */
            if (cmds[*num_cmds].input_file) { free(cmds[*num_cmds].input_file); cmds[*num_cmds].input_file = NULL; }
            if (cmds[*num_cmds].output_file) { free(cmds[*num_cmds].output_file); cmds[*num_cmds].output_file = NULL; }
        }

        cmd_str = strtok_r(NULL, "|", &saveptr);
    }

    return *num_cmds;
}

/*
 * setup_redirection: should be called in the child process before exec.
 * Handles input redirection only (reads the virtual file and duplicates a pipe
 * read end to STDIN).
 * Returns 0 on success, -1 on error.
 */
int setup_redirection(command *cmd) {
    if (!cmd) return 0;

    /* Input redirection: load file from VFS and feed into STDIN via pipe */
    if (cmd->input_file) {
        uint32_t entry_idx = fat_resolve_path(cmd->input_file);
        if (entry_idx == (uint32_t)-1) {
            fprintf(stderr, "mysh: %s: No such file\n", cmd->input_file);
            return -1;
        }

        char *content = fat_read_file(entry_idx);
        if (!content) {
            fprintf(stderr, "mysh: %s: Cannot read file\n", cmd->input_file);
            return -1;
        }

        int p[2];
        if (pipe(p) < 0) {
            free(content);
            return -1;
        }

        /* write content to write end, dup read end to STDIN in child */
        ssize_t total = strlen(content);
        ssize_t written = 0;
        while (written < total) {
            ssize_t w = write(p[1], content + written, total - written);
            if (w <= 0) break;
            written += w;
        }
        close(p[1]); /* close write end */

        if (dup2(p[0], STDIN_FILENO) < 0) {
            close(p[0]);
            free(content);
            return -1;
        }
        close(p[0]);
        free(content);
    }

    /* Note: output redirection for external commands is handled by the parent
       creating a pipe before forking and the child dup2()ing the pipe write-end
       onto STDOUT. For builtins, we capture output using dup/pipe in the parent. */

    return 0;
}

/* Helper to write captured buffer into virtual file (append or overwrite) */
static int write_buffer_to_vfs(const char *vpath, const char *buf, size_t len, int append) {
    if (!vpath) return -1;
    uint32_t entry_idx = fat_resolve_path(vpath);
    if (entry_idx == (uint32_t)-1) {
        /* try to create it */
        if (fat_touch(vpath) < 0) return -1;
        entry_idx = fat_resolve_path(vpath);
        if (entry_idx == (uint32_t)-1) return -1;
    }

    if (append) {
        char *existing = fat_read_file(entry_idx);
        if (existing) {
            size_t existing_len = strlen(existing);
            char *combined = malloc(existing_len + len);
            if (!combined) {
                free(existing);
                return -1;
            }
            memcpy(combined, existing, existing_len);
            memcpy(combined + existing_len, buf, len);
            fat_write_file(entry_idx, combined, existing_len + len);
            free(existing);
            free(combined);
            return 0;
        } else {
            /* file exists but read failed or empty -> just write new content */
            fat_write_file(entry_idx, buf, len);
            return 0;
        }
    } else {
        /* overwrite */
        fat_write_file(entry_idx, buf, len);
        return 0;
    }
}

/* Execute pipeline (supports up to 10 commands) */
int execute_pipeline(command *cmds, int num_cmds) {
    if (num_cmds <= 0) return 0;

    /* Single command special-case to handle builtins vs external and output redirection */
    if (num_cmds == 1) {
        command *c = &cmds[0];
        if (c->argc == 0) return 0;

        /* Builtin with possible output redirection */
        if (is_shell_builtin(c->argv[0])) {
            int output_pipe_fd = -1;
            int saved_stdout = -1;
            char *captured = NULL;
            size_t captured_len = 0;

            if (c->output_file) {
                /* create pipe to capture stdout of builtin */
                int p[2];
                if (pipe(p) < 0) {
                    perror("pipe");
                    return -1;
                }
                saved_stdout = dup(STDOUT_FILENO);
                dup2(p[1], STDOUT_FILENO);
                close(p[1]);
                output_pipe_fd = p[0];
            }

            /* For builtins, input redirection should be set in the current process:
               we emulate it by creating a pipe and dup2'ing it to stdin; but setup_redirection()
               expects to run in child; since builtin runs in parent we emulate here. */
            int in_saved = -1;
            int in_pipe_rd = -1;
            if (c->input_file) {
                uint32_t entry_idx = fat_resolve_path(c->input_file);
                if (entry_idx == (uint32_t)-1) {
                    fprintf(stderr, "mysh: %s: No such file\n", c->input_file);
                    /* restore stdout if changed */
                    if (output_pipe_fd >= 0) {
                        fflush(stdout);
                        dup2(saved_stdout, STDOUT_FILENO);
                        close(saved_stdout);
                        close(output_pipe_fd);
                    }
                    return -1;
                }
                char *content = fat_read_file(entry_idx);
                if (!content) {
                    fprintf(stderr, "mysh: %s: Cannot read file\n", c->input_file);
                    if (output_pipe_fd >= 0) {
                        fflush(stdout);
                        dup2(saved_stdout, STDOUT_FILENO);
                        close(saved_stdout);
                        close(output_pipe_fd);
                    }
                    return -1;
                }
                int p[2];
                if (pipe(p) < 0) {
                    free(content);
                    if (output_pipe_fd >= 0) {
                        fflush(stdout);
                        dup2(saved_stdout, STDOUT_FILENO);
                        close(saved_stdout);
                        close(output_pipe_fd);
                    }
                    return -1;
                }
                /* write content and dup read end onto stdin */
                ssize_t total = strlen(content);
                ssize_t written = 0;
                while (written < total) {
                    ssize_t w = write(p[1], content + written, total - written);
                    if (w <= 0) break;
                    written += w;
                }
                close(p[1]);
                free(content);
                in_saved = dup(STDIN_FILENO);
                dup2(p[0], STDIN_FILENO);
                in_pipe_rd = p[0];
                close(p[0]);
            }

            /* run builtin */
            int ret = do_shell_builtin(c->argc, c->argv);

            /* restore input if changed */
            if (in_saved >= 0) {
                fflush(stdin);
                dup2(in_saved, STDIN_FILENO);
                close(in_saved);
            }

            /* capture output and write into VFS if needed */
            if (output_pipe_fd >= 0) {
                fflush(stdout);
                dup2(saved_stdout, STDOUT_FILENO);
                close(saved_stdout);

                /* read captured output */
                char buf[4096];
                ssize_t r;
                while ((r = read(output_pipe_fd, buf, sizeof(buf))) > 0) {
                    /* accumulate and write directly to VFS piecewise to avoid huge reallocs */
                    write_buffer_to_vfs(c->output_file, buf, r, c->append);
                }
                close(output_pipe_fd);
            }

            return ret;
        }

        /* External single command */
        /* If output redirection present, create a pipe to capture child's stdout */
        int out_pipe_fd = -1;
        int pout[2] = {-1, -1};
        if (c->output_file) {
            if (pipe(pout) < 0) {
                perror("pipe");
                return -1;
            }
            /* parent will read pout[0]; child will dup2 pout[1] to stdout */
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            if (c->output_file) { close(pout[0]); close(pout[1]); }
            return -1;
        }

        if (pid == 0) {
            /* Child */
            /* Setup input redirection if any */
            if (setup_redirection(c) < 0) exit(1);

            /* If output redirection requested, dup pipe write end to stdout */
            if (c->output_file) {
                close(pout[0]);
                if (dup2(pout[1], STDOUT_FILENO) < 0) {
                    perror("dup2");
                    exit(1);
                }
                close(pout[1]);
            }

            execvp(c->argv[0], c->argv);
            fprintf(stderr, "%s: command not found\n", c->argv[0]);
            exit(127);
        } else {
            /* Parent */
            if (c->output_file) {
                close(pout[1]);
                out_pipe_fd = pout[0];
            }

            int status = 0;
            waitpid(pid, &status, 0);

            /* If we captured stdout, read and write it into VFS */
            if (out_pipe_fd >= 0) {
                char buf[4096];
                ssize_t r;
                while ((r = read(out_pipe_fd, buf, sizeof(buf))) > 0) {
                    write_buffer_to_vfs(c->output_file, buf, r, c->append);
                }
                close(out_pipe_fd);
            }

            return 0;
        }
    }

    /* Pipeline of N > 1 commands */
    int npipes = num_cmds - 1;
    int pipefds[2 * npipes];

    /* create pipes */
    for (int i = 0; i < npipes; i++) {
        if (pipe(pipefds + i*2) < 0) {
            perror("pipe");
            return -1;
        }
    }

    for (int i = 0; i < num_cmds; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            /* close all pipes */
            for (int j = 0; j < 2*npipes; j++) close(pipefds[j]);
            return -1;
        }

        if (pid == 0) {
            /* child */

            /* if not first, dup previous read end to stdin */
            if (i > 0) {
                if (dup2(pipefds[(i-1)*2], STDIN_FILENO) < 0) {
                    perror("dup2");
                    exit(1);
                }
            }

            /* if not last, dup write end to stdout */
            if (i < num_cmds - 1) {
                if (dup2(pipefds[i*2 + 1], STDOUT_FILENO) < 0) {
                    perror("dup2");
                    exit(1);
                }
            }

            /* close all pipe fds in child */
            for (int j = 0; j < 2*npipes; j++) close(pipefds[j]);

            /* Setup input redirection for first/this command if present */
            if (setup_redirection(&cmds[i]) < 0) exit(1);

            /* Execute builtin or external */
            if (is_shell_builtin(cmds[i].argv[0])) {
                do_shell_builtin(cmds[i].argc, cmds[i].argv);
                exit(0);
            } else {
                execvp(cmds[i].argv[0], cmds[i].argv);
                fprintf(stderr, "%s: command not found\n", cmds[i].argv[0]);
                exit(127);
            }
        }
        /* parent continues to spawn next */
    }

    /* parent closes all pipe fds */
    for (int j = 0; j < 2*npipes; j++) close(pipefds[j]);

    /* wait for children */
    for (int i = 0; i < num_cmds; i++) wait(NULL);

    return 0;
}
