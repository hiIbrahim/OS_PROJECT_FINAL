/* src/main.c */
#include "../include/common.h"
#include "../include/fat_fs.h"
#include "../include/shell.h"

/* Global variable definitions */
char ROOT_PATH[PATH_MAX];
volatile pid_t fg_pgid = 0;

/* Command history globals */
char *command_history[MAX_HISTORY];
int history_count = 0;
int history_index = 0;

/* Helper: dief */
void dief(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

/* Change into OS_PROJECT */
int change_into_os_project() {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return -1;
    char candidate[PATH_MAX];

    snprintf(candidate, sizeof(candidate), "%s/OS_PROJECT", cwd);
    if (access(candidate, R_OK | X_OK) == 0) {
        if (!realpath(candidate, ROOT_PATH)) return -1;
        if (chdir(ROOT_PATH) < 0) return -1;
        return 0;
    }

    char *home = getenv("HOME");
    if (home) {
        snprintf(candidate, sizeof(candidate), "%s/OS_PROJECT", home);
        if (access(candidate, R_OK | X_OK) == 0) {
            if (!realpath(candidate, ROOT_PATH)) return -1;
            if (chdir(ROOT_PATH) < 0) return -1;
            return 0;
        }
    }

    return -1;
}

/* Path Helpers */
int path_within_root(const char *resolved) {
    size_t rlen = strlen(ROOT_PATH);
    if (strncmp(resolved, ROOT_PATH, rlen) != 0) return 0;
    if (resolved[rlen] == '\0' || resolved[rlen] == '/') return 1;
    return 0;
}

/* History Management */
void add_to_history(const char *cmd) {
    if (!cmd || strlen(cmd) == 0) return;
    
    // Don't add duplicate consecutive commands
    if (history_count > 0 && strcmp(command_history[history_count - 1], cmd) == 0) {
        return;
    }
    
    if (history_count < MAX_HISTORY) {
        command_history[history_count++] = strdup(cmd);
    } else {
        // Shift history and add new command
        free(command_history[0]);
        for (int i = 0; i < MAX_HISTORY - 1; i++) {
            command_history[i] = command_history[i + 1];
        }
        command_history[MAX_HISTORY - 1] = strdup(cmd);
    }
    history_index = history_count;
}

void print_history() {
    for (int i = 0; i < history_count; i++) {
        printf("%4d  %s\n", i + 1, command_history[i]);
    }
}

void save_history() {
    char histpath[PATH_MAX];
    snprintf(histpath, sizeof(histpath), "%s/.mysh_history", ROOT_PATH);
    
    FILE *fp = fopen(histpath, "w");
    if (!fp) return;
    
    for (int i = 0; i < history_count; i++) {
        fprintf(fp, "%s\n", command_history[i]);
    }
    fclose(fp);
}

void load_history() {
    char histpath[PATH_MAX];
    snprintf(histpath, sizeof(histpath), "%s/.mysh_history", ROOT_PATH);
    
    FILE *fp = fopen(histpath, "r");
    if (!fp) return;
    
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    
    while ((read = getline(&line, &len, fp)) != -1) {
        if (read > 0 && line[read - 1] == '\n') line[read - 1] = '\0';
        if (strlen(line) > 0 && history_count < MAX_HISTORY) {
            command_history[history_count++] = strdup(line);
        }
    }
    
    free(line);
    fclose(fp);
    history_index = history_count;
}

/* Free memory allocated by parse_pipeline() for commands */
static void free_commands(command *cmds, int num_cmds) {
    if (!cmds) return;
    for (int i = 0; i < num_cmds; ++i) {
        /* free argv strings */
        for (int j = 0; j < cmds[i].argc && j < 64; ++j) {
            if (cmds[i].argv[j]) {
                free(cmds[i].argv[j]);
                cmds[i].argv[j] = NULL;
            }
        }
        cmds[i].argc = 0;

        /* free redirection strings */
        if (cmds[i].input_file) {
            free(cmds[i].input_file);
            cmds[i].input_file = NULL;
        }
        if (cmds[i].output_file) {
            free(cmds[i].output_file);
            cmds[i].output_file = NULL;
        }
        cmds[i].append = 0;
    }
}

/* Main loop */
int main() {
    if (change_into_os_project() < 0) {
        dief("OS_PROJECT folder not found.\n");
    }
    
    fat_init();
    load_history();
    
    char *line = NULL;
    size_t linecap = 0;
    
    printf("Welcome to MyShell! Type 'help' for available commands.\n");
    
    while (1) {
        printf("mysh:");
        fat_pwd();
        printf("$ ");
        fflush(stdout);
        
        ssize_t nread = getline(&line, &linecap, stdin);
        if (nread <= 0) {
            printf("\n");
            break;
        }
        
        if (nread > 0 && line[nread - 1] == '\n') line[nread - 1] = 0;
        if (strlen(line) == 0) continue;
        
        // Add to history
        add_to_history(line);
        
        // Parse and execute pipeline
        command cmds[10];
        int num_cmds = 0;
        
        // Make a copy since strtok modifies the string
        char *line_copy = strdup(line);
        if (!line_copy) continue;
        parse_pipeline(line_copy, cmds, &num_cmds);
        execute_pipeline(cmds, num_cmds);

        /* free strdup'd argv and redirection strings from parse_pipeline */
        free_commands(cmds, num_cmds);

        free(line_copy);
    }
    
    /* Save history before exit */
    save_history();

    /* Save VFS image before exiting */
    char imgpath[PATH_MAX];
    snprintf(imgpath, sizeof(imgpath), "%s/mysh_fs.img", ROOT_PATH);
    fat_save_image(imgpath);

    free(line);
    free(fs);
    
    /* Free history */
    for (int i = 0; i < history_count; i++) {
        free(command_history[i]);
    }
    
    return 0;
}
