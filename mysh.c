// mysh_rooted.c with FAT File System
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

/* ---------- FAT File System Configuration ---------- */
#define BLOCK_SIZE 512
#define MAX_BLOCKS 1024
#define MAX_FILENAME 255
#define FAT_EOC 0xFFFF  // End of chain marker
#define FAT_FREE 0x0000 // Free block marker

/* ---------- Globals ---------- */
char ROOT_PATH[PATH_MAX];
volatile pid_t fg_pgid = 0;

/* ---------- FAT Data Structures ---------- */
typedef struct {
    char name[MAX_FILENAME + 1];
    uint32_t size;
    uint16_t first_block;
    uint8_t is_dir;
    uint8_t is_used;
    time_t created;
    time_t modified;
    uint32_t parent_entry;  // Index of parent directory entry
} dir_entry;

typedef struct {
    uint16_t fat_table[MAX_BLOCKS];  // File Allocation Table
    dir_entry dir_entries[256];       // Directory entries (simple linear array)
    uint8_t blocks[MAX_BLOCKS][BLOCK_SIZE];  // Data blocks
    uint32_t num_entries;
    uint32_t current_dir;  // Current directory entry index
} fat_fs;

fat_fs *fs = NULL;

/* ---------- Helper: dief ---------- */
void dief(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

/* ---------- Change into OS_PROJECT ---------- */
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

/* ---------- Path Helpers ---------- */
int path_within_root(const char *resolved) {
    size_t rlen = strlen(ROOT_PATH);
    if (strncmp(resolved, ROOT_PATH, rlen) != 0) return 0;
    if (resolved[rlen] == '\0' || resolved[rlen] == '/') return 1;
    return 0;
}

/* ---------- FAT File System Implementation ---------- */

int fat_save_image(const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) return -1;
    
    size_t written = fwrite(fs, sizeof(fat_fs), 1, fp);
    fclose(fp);
    
    return (written == 1) ? 0 : -1;
}

int fat_load_image(const char *filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) return -1;
    
    if (!fs) fs = malloc(sizeof(fat_fs));
    size_t read = fread(fs, sizeof(fat_fs), 1, fp);
    fclose(fp);
    
    return (read == 1) ? 0 : -1;
}

void fat_init() {
    char imgpath[PATH_MAX];
    snprintf(imgpath, sizeof(imgpath), "%s/mysh_fs.img", ROOT_PATH);
    
    // Try to load existing image
    if (fat_load_image(imgpath) == 0) {
        printf("Loaded existing file system from mysh_fs.img\n");
        return;
    }
    
    // Create new file system
    printf("Creating new file system...\n");
    fs = calloc(1, sizeof(fat_fs));
    
    // Initialize FAT table (all blocks free)
    for (int i = 0; i < MAX_BLOCKS; i++) {
        fs->fat_table[i] = FAT_FREE;
    }
    
    // Create root directory entry
    dir_entry *root = &fs->dir_entries[0];
    strcpy(root->name, "/");
    root->size = 0;
    root->first_block = FAT_EOC;
    root->is_dir = 1;
    root->is_used = 1;
    root->created = time(NULL);
    root->modified = time(NULL);
    root->parent_entry = 0;
    
    fs->num_entries = 1;
    fs->current_dir = 0;
    
    // Create a sample readme.txt file
    dir_entry *readme = &fs->dir_entries[1];
    strcpy(readme->name, "readme.txt");
    const char *content = "This is a virtual FAT file system.\nWelcome to mysh!\n";
    readme->size = strlen(content);
    readme->first_block = 0;
    readme->is_dir = 0;
    readme->is_used = 1;
    readme->created = time(NULL);
    readme->modified = time(NULL);
    readme->parent_entry = 0;
    
    // Write content to first block
    strncpy((char *)fs->blocks[0], content, BLOCK_SIZE);
    fs->fat_table[0] = FAT_EOC;
    
    fs->num_entries = 2;
    
    // Save initial state
    fat_save_image(imgpath);
}

uint16_t fat_alloc_block() {
    for (uint16_t i = 0; i < MAX_BLOCKS; i++) {
        if (fs->fat_table[i] == FAT_FREE) {
            fs->fat_table[i] = FAT_EOC;
            return i;
        }
    }
    return FAT_EOC;
}

void fat_free_chain(uint16_t start_block) {
    uint16_t current = start_block;
    while (current != FAT_EOC && current < MAX_BLOCKS) {
        uint16_t next = fs->fat_table[current];
        fs->fat_table[current] = FAT_FREE;
        memset(fs->blocks[current], 0, BLOCK_SIZE);
        current = next;
    }
}

uint32_t fat_find_entry(const char *name, uint32_t parent) {
    for (uint32_t i = 0; i < fs->num_entries; i++) {
        if (fs->dir_entries[i].is_used && 
            fs->dir_entries[i].parent_entry == parent &&
            strcmp(fs->dir_entries[i].name, name) == 0) {
            return i;
        }
    }
    return (uint32_t)-1;
}

uint32_t fat_resolve_path(const char *path) {
    if (!path || path[0] == '\0') return fs->current_dir;
    if (strcmp(path, "/") == 0) return 0;
    
    uint32_t current = (path[0] == '/') ? 0 : fs->current_dir;
    char *copy = strdup(path);
    char *tok = strtok(copy, "/");
    
    while (tok) {
        if (strcmp(tok, ".") == 0) {
            tok = strtok(NULL, "/");
            continue;
        }
        if (strcmp(tok, "..") == 0) {
            current = fs->dir_entries[current].parent_entry;
            tok = strtok(NULL, "/");
            continue;
        }
        
        uint32_t next = fat_find_entry(tok, current);
        if (next == (uint32_t)-1) {
            free(copy);
            return (uint32_t)-1;
        }
        current = next;
        tok = strtok(NULL, "/");
    }
    
    free(copy);
    return current;
}

int fat_mkdir(const char *path) {
    if (!path) return -1;
    
    char *copy = strdup(path);
    char *last = strrchr(copy, '/');
    char *name;
    uint32_t parent;
    
    if (!last) {
        parent = fs->current_dir;
        name = copy;
    } else {
        *last = '\0';
        parent = fat_resolve_path(copy);
        name = last + 1;
        if (parent == (uint32_t)-1) {
            free(copy);
            return -1;
        }
    }
    
    // Check if already exists
    if (fat_find_entry(name, parent) != (uint32_t)-1) {
        free(copy);
        errno = EEXIST;
        return -1;
    }
    
    // Check if we have space for more entries
    if (fs->num_entries >= 256) {
        free(copy);
        errno = ENOSPC;
        return -1;
    }
    
    // Create new directory entry
    dir_entry *new_dir = &fs->dir_entries[fs->num_entries];
    strncpy(new_dir->name, name, MAX_FILENAME);
    new_dir->size = 0;
    new_dir->first_block = FAT_EOC;
    new_dir->is_dir = 1;
    new_dir->is_used = 1;
    new_dir->created = time(NULL);
    new_dir->modified = time(NULL);
    new_dir->parent_entry = parent;
    
    fs->num_entries++;
    free(copy);
    return 0;
}

int fat_touch(const char *path) {
    if (!path) return -1;
    
    char *copy = strdup(path);
    char *last = strrchr(copy, '/');
    char *name;
    uint32_t parent;
    
    if (!last) {
        parent = fs->current_dir;
        name = copy;
    } else {
        *last = '\0';
        parent = fat_resolve_path(copy);
        name = last + 1;
        if (parent == (uint32_t)-1) {
            free(copy);
            return -1;
        }
    }
    
    // Check if already exists
    uint32_t existing = fat_find_entry(name, parent);
    if (existing != (uint32_t)-1) {
        if (fs->dir_entries[existing].is_dir) {
            free(copy);
            errno = EISDIR;
            return -1;
        }
        // Update modification time
        fs->dir_entries[existing].modified = time(NULL);
        free(copy);
        return 0;
    }
    
    // Check if we have space
    if (fs->num_entries >= 256) {
        free(copy);
        errno = ENOSPC;
        return -1;
    }
    
    // Create new file entry
    dir_entry *new_file = &fs->dir_entries[fs->num_entries];
    strncpy(new_file->name, name, MAX_FILENAME);
    new_file->size = 0;
    new_file->first_block = FAT_EOC;
    new_file->is_dir = 0;
    new_file->is_used = 1;
    new_file->created = time(NULL);
    new_file->modified = time(NULL);
    new_file->parent_entry = parent;
    
    fs->num_entries++;
    
    // DON'T create empty real file here anymore
    // Let the editor create it naturally
    
    free(copy);
    return 0;
}

int fat_write_file(uint32_t entry_idx, const char *data, size_t size) {
    if (entry_idx >= fs->num_entries || !fs->dir_entries[entry_idx].is_used) {
        return -1;
    }
    
    dir_entry *entry = &fs->dir_entries[entry_idx];
    if (entry->is_dir) return -1;
    
    // Free existing blocks
    if (entry->first_block != FAT_EOC) {
        fat_free_chain(entry->first_block);
    }
    
    if (size == 0) {
        entry->first_block = FAT_EOC;
        entry->size = 0;
        entry->modified = time(NULL);
        return 0;
    }
    
    // Allocate blocks for new data
    size_t blocks_needed = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint16_t first = FAT_EOC, prev = FAT_EOC;
    
    for (size_t i = 0; i < blocks_needed; i++) {
        uint16_t block = fat_alloc_block();
        if (block == FAT_EOC) {
            // Out of space, free what we allocated
            if (first != FAT_EOC) fat_free_chain(first);
            return -1;
        }
        
        size_t offset = i * BLOCK_SIZE;
        size_t to_copy = (size - offset > BLOCK_SIZE) ? BLOCK_SIZE : (size - offset);
        memcpy(fs->blocks[block], data + offset, to_copy);
        
        if (first == FAT_EOC) {
            first = block;
        } else {
            fs->fat_table[prev] = block;
        }
        prev = block;
    }
    
    entry->first_block = first;
    entry->size = size;
    entry->modified = time(NULL);
    return 0;
}

char* fat_read_file(uint32_t entry_idx) {
    if (entry_idx >= fs->num_entries || !fs->dir_entries[entry_idx].is_used) {
        return NULL;
    }
    
    dir_entry *entry = &fs->dir_entries[entry_idx];
    if (entry->is_dir) return NULL;
    if (entry->size == 0) return strdup("");
    
    char *data = malloc(entry->size + 1);
    size_t offset = 0;
    uint16_t current = entry->first_block;
    
    while (current != FAT_EOC && offset < entry->size) {
        size_t to_copy = (entry->size - offset > BLOCK_SIZE) ? BLOCK_SIZE : (entry->size - offset);
        memcpy(data + offset, fs->blocks[current], to_copy);
        offset += to_copy;
        current = fs->fat_table[current];
    }
    
    data[entry->size] = '\0';
    return data;
}

void fat_ls(const char *path) {
    uint32_t dir_idx = path ? fat_resolve_path(path) : fs->current_dir;
    
    if (dir_idx == (uint32_t)-1) {
        fprintf(stderr, "ls: no such directory: %s\n", path ? path : "");
        return;
    }
    
    if (!fs->dir_entries[dir_idx].is_dir) {
        printf("%s\n", fs->dir_entries[dir_idx].name);
        return;
    }
    
    // List all entries in this directory
    for (uint32_t i = 0; i < fs->num_entries; i++) {
        if (fs->dir_entries[i].is_used && 
            fs->dir_entries[i].parent_entry == dir_idx) {
            printf("%s%s  ", 
                   fs->dir_entries[i].name,
                   fs->dir_entries[i].is_dir ? "/" : "");
        }
    }
    printf("\n");
}

void fat_cat(const char *path) {
    uint32_t entry_idx = fat_resolve_path(path);
    
    if (entry_idx == (uint32_t)-1) {
        fprintf(stderr, "cat: no such file: %s\n", path ? path : "");
        return;
    }
    
    if (fs->dir_entries[entry_idx].is_dir) {
        fprintf(stderr, "cat: is a directory: %s\n", path);
        return;
    }
    
    char *content = fat_read_file(entry_idx);
    if (content) {
        printf("%s", content);
        free(content);
    }
}

int fat_cd(const char *path) {
    if (!path) {
        fs->current_dir = 0;
        return 0;
    }
    
    uint32_t dir_idx = fat_resolve_path(path);
    if (dir_idx == (uint32_t)-1) {
        errno = ENOENT;
        return -1;
    }
    
    if (!fs->dir_entries[dir_idx].is_dir) {
        errno = ENOTDIR;
        return -1;
    }
    
    fs->current_dir = dir_idx;
    return 0;
}

void fat_pwd() {
    if (fs->current_dir == 0) {
        printf("/\n");
        return;
    }
    
    char path[PATH_MAX] = {0};
    uint32_t idx = fs->current_dir;
    
    while (idx != 0) {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "/%s%s", fs->dir_entries[idx].name, path);
        strcpy(path, tmp);
        idx = fs->dir_entries[idx].parent_entry;
    }
    
    printf("%s\n", path);
}

void fat_sync_from_real_file(const char *path) {
    char realfile[PATH_MAX];
    snprintf(realfile, sizeof(realfile), "%s/%s", ROOT_PATH, path);
    
    // Check if file exists on disk
    if (access(realfile, F_OK) != 0) {
        return;
    }
    
    uint32_t entry_idx = fat_resolve_path(path);
    
    // If file doesn't exist in VFS, create it
    if (entry_idx == (uint32_t)-1) {
        printf("[VFS] Auto-creating '%s' in virtual file system\n", path);
        if (fat_touch(path) < 0) {
            fprintf(stderr, "[VFS] Failed to create '%s' in virtual file system\n", path);
            return;
        }
        entry_idx = fat_resolve_path(path);
        if (entry_idx == (uint32_t)-1) return;
    }
    
    if (fs->dir_entries[entry_idx].is_dir) {
        return;
    }
    
    FILE *fp = fopen(realfile, "r");
    if (!fp) return;
    
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);
    
    if (size > 0) {
        char *buf = malloc(size);
        fread(buf, 1, size, fp);
        fclose(fp);
        
        fat_write_file(entry_idx, buf, size);
        free(buf);
        printf("[VFS] Synced '%s' to virtual file system (%ld bytes)\n", path, size);
    } else {
        fclose(fp);
    }
}

/* ---------- Shell builtins ---------- */
int is_shell_builtin(const char *cmd) {
    return (strcmp(cmd, "cd") == 0 || strcmp(cmd, "exit") == 0 || 
            strcmp(cmd, "history") == 0 || strcmp(cmd, "jobs") == 0 ||
            strcmp(cmd, "ls") == 0 || strcmp(cmd, "cat") == 0 ||
            strcmp(cmd, "mkdir") == 0 || strcmp(cmd, "touch") == 0 ||
            strcmp(cmd, "pwd") == 0 || strcmp(cmd, "grep") == 0);
}

void fat_grep(const char *pattern, const char *filename) {
    if (!pattern || !filename) {
        fprintf(stderr, "grep: missing operand\n");
        return;
    }
    
    uint32_t entry_idx = fat_resolve_path(filename);
    if (entry_idx == (uint32_t)-1) {
        fprintf(stderr, "grep: %s: No such file\n", filename);
        return;
    }
    
    if (fs->dir_entries[entry_idx].is_dir) {
        fprintf(stderr, "grep: %s: Is a directory\n", filename);
        return;
    }
    
    char *content = fat_read_file(entry_idx);
    if (!content) return;
    
    // Simple line-by-line grep
    char *line = strtok(content, "\n");
    while (line) {
        if (strstr(line, pattern)) {
            printf("%s\n", line);
        }
        line = strtok(NULL, "\n");
    }
    
    free(content);
}

int do_shell_builtin(int argc, char **argv) {
    if (argc == 0) return 0;
    
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
        // Save file system before exiting
        char imgpath[PATH_MAX];
        snprintf(imgpath, sizeof(imgpath), "%s/mysh_fs.img", ROOT_PATH);
        fat_save_image(imgpath);
        printf("File system saved to mysh_fs.img\n");
        exit(0);
    }
    else if (strcmp(argv[0], "history") == 0) {
        return 0;
    }
    else if (strcmp(argv[0], "jobs") == 0) {
        return 0;
    }
    
    return -1;
}

/* ---------- Parse and execute command with pipes ---------- */
typedef struct {
    char *argv[64];
    int argc;
} command;

int parse_pipeline(char *line, command *cmds, int *num_cmds) {
    *num_cmds = 0;
    char *cmd_str = strtok(line, "|");
    
    while (cmd_str && *num_cmds < 10) {
        // Trim leading spaces
        while (*cmd_str == ' ') cmd_str++;
        
        cmds[*num_cmds].argc = 0;
        char *tok = strtok(cmd_str, " ");
        while (tok && cmds[*num_cmds].argc < 63) {
            cmds[*num_cmds].argv[cmds[*num_cmds].argc++] = tok;
            tok = strtok(NULL, " ");
        }
        cmds[*num_cmds].argv[cmds[*num_cmds].argc] = NULL;
        
        if (cmds[*num_cmds].argc > 0) {
            (*num_cmds)++;
        }
        
        cmd_str = strtok(NULL, "|");
    }
    
    return *num_cmds;
}

int execute_pipeline(command *cmds, int num_cmds) {
    if (num_cmds == 0) return 0;
    
    // Single command (no pipe)
    if (num_cmds == 1) {
        if (is_shell_builtin(cmds[0].argv[0])) {
            return do_shell_builtin(cmds[0].argc, cmds[0].argv);
        }
        
        pid_t pid = fork();
        if (pid == 0) {
            execvp(cmds[0].argv[0], cmds[0].argv);
            fprintf(stderr, "%s: command not found\n", cmds[0].argv[0]);
            exit(127);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            
            // Check if editor was used
            int is_editor = (strcmp(cmds[0].argv[0], "nano") == 0 || 
                           strcmp(cmds[0].argv[0], "vim") == 0 ||
                           strcmp(cmds[0].argv[0], "vi") == 0 ||
                           strcmp(cmds[0].argv[0], "emacs") == 0);
            
            if (is_editor) {
                for (int i = 1; i < cmds[0].argc; i++) {
                    if (cmds[0].argv[i][0] != '-') {
                        fat_sync_from_real_file(cmds[0].argv[i]);
                    }
                }
                char imgpath[PATH_MAX];
                snprintf(imgpath, sizeof(imgpath), "%s/mysh_fs.img", ROOT_PATH);
                fat_save_image(imgpath);
            }
            return 0;
        }
        return -1;
    }
    
    // Pipeline execution
    int pipefds[2 * (num_cmds - 1)];
    
    // Create all pipes
    for (int i = 0; i < num_cmds - 1; i++) {
        if (pipe(pipefds + i * 2) < 0) {
            perror("pipe");
            return -1;
        }
    }
    
    for (int i = 0; i < num_cmds; i++) {
        pid_t pid = fork();
        
        if (pid == 0) {
            // Not first command: read from previous pipe
            if (i > 0) {
                dup2(pipefds[(i - 1) * 2], STDIN_FILENO);
            }
            
            // Not last command: write to next pipe
            if (i < num_cmds - 1) {
                dup2(pipefds[i * 2 + 1], STDOUT_FILENO);
            }
            
            // Close all pipe fds
            for (int j = 0; j < 2 * (num_cmds - 1); j++) {
                close(pipefds[j]);
            }
            
            // Execute builtin or external
            if (is_shell_builtin(cmds[i].argv[0])) {
                do_shell_builtin(cmds[i].argc, cmds[i].argv);
                exit(0);
            } else {
                execvp(cmds[i].argv[0], cmds[i].argv);
                fprintf(stderr, "%s: command not found\n", cmds[i].argv[0]);
                exit(127);
            }
        } else if (pid < 0) {
            perror("fork");
            return -1;
        }
    }
    
    // Parent closes all pipes
    for (int i = 0; i < 2 * (num_cmds - 1); i++) {
        close(pipefds[i]);
    }
    
    // Wait for all children
    for (int i = 0; i < num_cmds; i++) {
        wait(NULL);
    }
    
    return 0;
}

/* ---------- Main loop ---------- */
int main() {
    if (change_into_os_project() < 0) {
        dief("OS_PROJECT folder not found.\n");
    }
    
    fat_init();
    
    char *line = NULL;
    size_t linecap = 0;
    
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
        
        if (line[nread - 1] == '\n') line[nread - 1] = 0;
        if (strlen(line) == 0) continue;
        
        // Parse and execute pipeline
        command cmds[10];
        int num_cmds;
        
        // Make a copy since strtok modifies the string
        char *line_copy = strdup(line);
        parse_pipeline(line_copy, cmds, &num_cmds);
        execute_pipeline(cmds, num_cmds);
        free(line_copy);
    }
    
    free(line);
    free(fs);
    return 0;
}
