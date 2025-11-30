#include "../include/shell.h"
#include "../include/fat_fs.h"
#include "../include/common.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

int is_shell_builtin(const char *cmd) {
    if (!cmd) return 0;
    return (strcmp(cmd, "cd") == 0 || strcmp(cmd, "exit") == 0 ||
            strcmp(cmd, "history") == 0 || strcmp(cmd, "jobs") == 0 ||
            strcmp(cmd, "ls") == 0 || strcmp(cmd, "cat") == 0 ||
            strcmp(cmd, "mkdir") == 0 || strcmp(cmd, "touch") == 0 ||
            strcmp(cmd, "pwd") == 0 || strcmp(cmd, "grep") == 0);
}

int do_shell_builtin(int argc, char **argv) {
    if (argc == 0 || !argv || !argv[0]) return 0;

    if (strcmp(argv[0], "cd") == 0) {
        const char *path = (argc > 1) ? argv[1] : "/";
        if (fat_cd(path) < 0) {
            perror("cd");
            return -1;
        }
        return 0;
    }
    else if (strcmp(argv[0], "ls") == 0) {
        fat_ls(argc > 1 ? argv[1] : NULL);
        return 0;
    }
    else if (strcmp(argv[0], "cat") == 0) {
        if (argc < 2) {
            fprintf(stderr, "cat: missing operand\n");
            return -1;
        }
        fat_cat(argv[1]);
        return 0;
    }
    else if (strcmp(argv[0], "grep") == 0) {
        if (argc < 3) {
            fprintf(stderr, "grep: usage: grep pattern file\n");
            return -1;
        }
        fat_grep(argv[1], argv[2]);
        return 0;
    }
    else if (strcmp(argv[0], "mkdir") == 0) {
        if (argc < 2) {
            fprintf(stderr, "mkdir: missing operand\n");
            return -1;
        }
        if (fat_mkdir(argv[1]) < 0) {
            perror("mkdir");
            return -1;
        }
        return 0;
    }
    else if (strcmp(argv[0], "touch") == 0) {
        if (argc < 2) {
            fprintf(stderr, "touch: missing operand\n");
            return -1;
        }
        if (fat_touch(argv[1]) < 0) {
            perror("touch");
            return -1;
        }
        return 0;
    }
    else if (strcmp(argv[0], "pwd") == 0) {
        fat_pwd();
        return 0;
    }
    else if (strcmp(argv[0], "exit") == 0) {
        /* Save file system before exiting */
        char imgpath[PATH_MAX];
        snprintf(imgpath, sizeof(imgpath), "%s/mysh_fs.img", ROOT_PATH);
        fat_save_image(imgpath);
        printf("File system saved to mysh_fs.img\n");
        exit(0);
    }
    else if (strcmp(argv[0], "history") == 0) {
        print_history();
        return 0;
    }
    else if (strcmp(argv[0], "jobs") == 0) {
        /* Not implemented: placeholder for job control */
        printf("jobs: no background jobs support\n");
        return 0;
    }

    return -1;
}
