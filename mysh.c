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
        fflush(stdout);
        return;
    }
    
    // List all entries in this directory - one per line
    for (uint32_t i = 0; i < fs->num_entries; i++) {
        if (fs->dir_entries[i].is_used && 
            fs->dir_entries[i].parent_entry == dir_idx) {
            printf("%s%s\n", 
                   fs->dir_entries[i].name,
                   fs->dir_entries[i].is_dir ? "/" : "");
            fflush(stdout);  // Flush after each line
        }
    }
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
            strcmp(cmd, "pwd") == 0 || strcmp(cmd, "grep") == 0 ||
            strcmp(cmd, "rm") == 0 || strcmp(cmd, "rmdir") == 0 ||
            strcmp(cmd, "head") == 0 || strcmp(cmd, "tail") == 0 ||
            strcmp(cmd, "mv") == 0);
}

int fat_mv(const char *source, const char *dest) {
    if (!source || !dest) {
        fprintf(stderr, "mv: missing operand\n");
        return -1;
    }
    
    // Find source entry
    uint32_t src_idx = fat_resolve_path(source);
    if (src_idx == (uint32_t)-1) {
        fprintf(stderr, "mv: cannot stat '%s': No such file or directory\n", source);
        return -1;
    }
    
    // Cannot move root
    if (src_idx == 0) {
        fprintf(stderr, "mv: cannot move root directory\n");
        return -1;
    }
    
    dir_entry *src_entry = &fs->dir_entries[src_idx];
    
    // Parse destination path
    char *dest_copy = strdup(dest);
    char *last_slash = strrchr(dest_copy, '/');
    char *new_name;
    uint32_t dest_parent_idx;
    
    if (!last_slash) {
        // No slash - destination is in current directory
        dest_parent_idx = fs->current_dir;
        new_name = dest_copy;
    } else {
        // Has slash - split into parent and name
        *last_slash = '\0';
        new_name = last_slash + 1;
        
        if (strlen(dest_copy) == 0) {
            // Destination is root
            dest_parent_idx = 0;
        } else {
            dest_parent_idx = fat_resolve_path(dest_copy);
            if (dest_parent_idx == (uint32_t)-1) {
                fprintf(stderr, "mv: cannot move '%s' to '%s': No such directory\n", source, dest);
                free(dest_copy);
                return -1;
            }
        }
    }
    
    // Check if destination parent is a directory
    if (!fs->dir_entries[dest_parent_idx].is_dir) {
        fprintf(stderr, "mv: cannot move to '%s': Not a directory\n", dest);
        free(dest_copy);
        return -1;
    }
    
    // Check if destination already exists
    uint32_t existing = fat_find_entry(new_name, dest_parent_idx);
    if (existing != (uint32_t)-1) {
        // If destination exists and is a directory, move source into it
        if (fs->dir_entries[existing].is_dir) {
            dest_parent_idx = existing;
            new_name = src_entry->name;
            
            // Check again if it exists in the destination directory
            uint32_t existing2 = fat_find_entry(new_name, dest_parent_idx);
            if (existing2 != (uint32_t)-1) {
                fprintf(stderr, "mv: cannot move '%s' to '%s': File exists\n", source, dest);
                free(dest_copy);
                return -1;
            }
        } else {
            fprintf(stderr, "mv: cannot move '%s' to '%s': File exists\n", source, dest);
            free(dest_copy);
            return -1;
        }
    }
    
    // Perform the move - update parent and name
    src_entry->parent_entry = dest_parent_idx;
    strncpy(src_entry->name, new_name, MAX_FILENAME);
    src_entry->modified = time(NULL);
    
    // Try to move real file/directory if it exists
    char real_src[PATH_MAX], real_dest[PATH_MAX];
    snprintf(real_src, sizeof(real_src), "%s/%s", ROOT_PATH, source);
    snprintf(real_dest, sizeof(real_dest), "%s/%s", ROOT_PATH, dest);
    rename(real_src, real_dest);  // Ignore errors if files don't exist
    
    printf("Moved '%s' to '%s'\n", source, dest);
    free(dest_copy);
    
    return 0;
}

void fat_head(int num_lines, const char *filename) {
    if (!filename) {
        fprintf(stderr, "head: missing file operand\n");
        return;
    }
    
    uint32_t entry_idx = fat_resolve_path(filename);
    if (entry_idx == (uint32_t)-1) {
        fprintf(stderr, "head: %s: No such file\n", filename);
        return;
    }
    
    if (fs->dir_entries[entry_idx].is_dir) {
        fprintf(stderr, "head: %s: Is a directory\n", filename);
        return;
    }
    
    char *content = fat_read_file(entry_idx);
    if (!content) return;
    
    // Split into lines and print first N
    char *content_copy = strdup(content);
    char *line = strtok(content_copy, "\n");
    int count = 0;
    
    while (line && count < num_lines) {
        printf("%s\n", line);
        count++;
        line = strtok(NULL, "\n");
    }
    
    free(content_copy);
    free(content);
}

void fat_tail(int num_lines, const char *filename) {
    if (!filename) {
        fprintf(stderr, "tail: missing file operand\n");
        return;
    }
    
    uint32_t entry_idx = fat_resolve_path(filename);
    if (entry_idx == (uint32_t)-1) {
        fprintf(stderr, "tail: %s: No such file\n", filename);
        return;
    }
    
    if (fs->dir_entries[entry_idx].is_dir) {
        fprintf(stderr, "tail: %s: Is a directory\n", filename);
        return;
    }
    
    char *content = fat_read_file(entry_idx);
    if (!content) return;
    
    // Count total lines first
    char *content_copy1 = strdup(content);
    char *line = strtok(content_copy1, "\n");
    int total_lines = 0;
    while (line) {
        total_lines++;
        line = strtok(NULL, "\n");
    }
    free(content_copy1);
    
    // Calculate starting line
    int start_line = (total_lines > num_lines) ? (total_lines - num_lines) : 0;
    
    // Print from start_line to end
    char *content_copy2 = strdup(content);
    line = strtok(content_copy2, "\n");
    int count = 0;
    
    while (line) {
        if (count >= start_line) {
            printf("%s\n", line);
        }
        count++;
        line = strtok(NULL, "\n");
    }
    
    free(content_copy2);
    free(content);
}

int fat_rmdir(const char *path) {
    if (!path) {
        fprintf(stderr, "rmdir: missing operand\n");
        return -1;
    }
    
    uint32_t entry_idx = fat_resolve_path(path);
    if (entry_idx == (uint32_t)-1) {
        fprintf(stderr, "rmdir: failed to remove '%s': No such file or directory\n", path);
        return -1;
    }
    
    dir_entry *entry = &fs->dir_entries[entry_idx];
    
    // Must be a directory
    if (!entry->is_dir) {
        fprintf(stderr, "rmdir: failed to remove '%s': Not a directory\n", path);
        return -1;
    }
    
    // Cannot remove root directory
    if (entry_idx == 0) {
        fprintf(stderr, "rmdir: failed to remove '/': Cannot remove root directory\n");
        return -1;
    }
    
    // Check if directory is empty (no children)
    for (uint32_t i = 0; i < fs->num_entries; i++) {
        if (fs->dir_entries[i].is_used && 
            fs->dir_entries[i].parent_entry == entry_idx) {
            fprintf(stderr, "rmdir: failed to remove '%s': Directory not empty\n", path);
            return -1;
        }
    }
    
    // Cannot remove current directory
    if (entry_idx == fs->current_dir) {
        fprintf(stderr, "rmdir: failed to remove '%s': Cannot remove current directory\n", path);
        return -1;
    }
    
    // Mark entry as unused
    entry->is_used = 0;
    
    // Try to remove real directory if it exists
    char realdir[PATH_MAX];
    snprintf(realdir, sizeof(realdir), "%s/%s", ROOT_PATH, path);
    rmdir(realdir);  // Ignore errors if directory doesn't exist
    
    printf("Removed directory '%s' from virtual file system\n", path);
    
    return 0;
}

int fat_rm(const char *path) {
    if (!path) {
        fprintf(stderr, "rm: missing operand\n");
        return -1;
    }
    
    uint32_t entry_idx = fat_resolve_path(path);
    if (entry_idx == (uint32_t)-1) {
        fprintf(stderr, "rm: cannot remove '%s': No such file or directory\n", path);
        return -1;
    }
    
    dir_entry *entry = &fs->dir_entries[entry_idx];
    
    // Don't allow removing directories (use rmdir for that)
    if (entry->is_dir) {
        fprintf(stderr, "rm: cannot remove '%s': Is a directory\n", path);
        return -1;
    }
    
    // Free the blocks used by this file
    if (entry->first_block != FAT_EOC) {
        fat_free_chain(entry->first_block);
    }
    
    // Mark entry as unused
    entry->is_used = 0;
    entry->first_block = FAT_EOC;
    entry->size = 0;
    
    // Try to remove the real file too (if it exists)
    char realfile[PATH_MAX];
    snprintf(realfile, sizeof(realfile), "%s/%s", ROOT_PATH, path);
    unlink(realfile);  // Ignore errors if file doesn't exist
    
    printf("Removed '%s' from virtual file system\n", path);
    
    return 0;
}

void fat_grep(const char *pattern, const char *filename) {
    if (!pattern) {
        fprintf(stderr, "grep: missing pattern\n");
        return;
    }
    
    // If reading from stdin (no filename or filename is "-")
    if (!filename || strcmp(filename, "-") == 0) {
        char line[1024];
        while (fgets(line, sizeof(line), stdin)) {
            // Remove trailing newline if present
            size_t len = strlen(line);
            if (len > 0 && line[len-1] == '\n') {
                line[len-1] = '\0';
            }
            
            // Only print if pattern is found
            if (strstr(line, pattern) != NULL) {
                printf("%s\n", line);
                fflush(stdout);
            }
        }
        return;
    }
    
    // Reading from a file
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
    
    // Make a copy since strtok modifies the string
    char *content_copy = strdup(content);
    char *line = strtok(content_copy, "\n");
    while (line) {
        if (strstr(line, pattern) != NULL) {
            printf("%s\n", line);
        }
        line = strtok(NULL, "\n");
    }
    
    free(content_copy);
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
        if (argc < 2) {
            fprintf(stderr, "grep: usage: grep pattern [file]\n");
            return -1;
        }
        // grep pattern [file]
        // If no file, read from stdin
        fat_grep(argv[1], argc > 2 ? argv[2] : NULL);
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
    else if (strcmp(argv[0], "rm") == 0) {
        if (argc < 2) {
            fprintf(stderr, "rm: missing operand\n");
            return -1;
        }
        if (fat_rm(argv[1]) < 0) {
            return -1;
        }
        // Auto-save after removing
        char imgpath[PATH_MAX];
        snprintf(imgpath, sizeof(imgpath), "%s/mysh_fs.img", ROOT_PATH);
        fat_save_image(imgpath);
        return 0;
    }
    else if (strcmp(argv[0], "rmdir") == 0) {
        if (argc < 2) {
            fprintf(stderr, "rmdir: missing operand\n");
            return -1;
        }
        if (fat_rmdir(argv[1]) < 0) {
            return -1;
        }
        // Auto-save after removing directory
        char imgpath[PATH_MAX];
        snprintf(imgpath, sizeof(imgpath), "%s/mysh_fs.img", ROOT_PATH);
        fat_save_image(imgpath);
        return 0;
    }
    else if (strcmp(argv[0], "head") == 0) {
        int num_lines = 10;  // Default: 10 lines
        const char *filename = NULL;
        
        // Parse arguments: head [-n NUM] FILE
        if (argc < 2) {
            fprintf(stderr, "head: missing file operand\n");
            return -1;
        }
        
        if (argc == 2) {
            // head FILE
            filename = argv[1];
        } else if (argc == 4 && strcmp(argv[1], "-n") == 0) {
            // head -n NUM FILE
            num_lines = atoi(argv[2]);
            filename = argv[3];
        } else if (argc == 3 && argv[1][0] == '-' && isdigit(argv[1][1])) {
            // head -NUM FILE
            num_lines = atoi(argv[1] + 1);
            filename = argv[2];
        } else {
            filename = argv[1];
        }
        
        fat_head(num_lines, filename);
        return 0;
    }
    else if (strcmp(argv[0], "tail") == 0) {
        int num_lines = 10;  // Default: 10 lines
        const char *filename = NULL;
        
        // Parse arguments: tail [-n NUM] FILE
        if (argc < 2) {
            fprintf(stderr, "tail: missing file operand\n");
            return -1;
        }
        
        if (argc == 2) {
            // tail FILE
            filename = argv[1];
        } else if (argc == 4 && strcmp(argv[1], "-n") == 0) {
            // tail -n NUM FILE
            num_lines = atoi(argv[2]);
            filename = argv[3];
        } else if (argc == 3 && argv[1][0] == '-' && isdigit(argv[1][1])) {
            // tail -NUM FILE
            num_lines = atoi(argv[1] + 1);
            filename = argv[2];
        } else {
            filename = argv[1];
        }
        
        fat_tail(num_lines, filename);
        return 0;
    }
    else if (strcmp(argv[0], "mv") == 0) {
        if (argc < 3) {
            fprintf(stderr, "mv: missing operand\n");
            fprintf(stderr, "Usage: mv SOURCE DEST\n");
            return -1;
        }
        if (fat_mv(argv[1], argv[2]) < 0) {
            return -1;
        }
        // Auto-save after moving
        char imgpath[PATH_MAX];
        snprintf(imgpath, sizeof(imgpath), "%s/mysh_fs.img", ROOT_PATH);
        fat_save_image(imgpath);
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
    
    // Check if there's a pipe at all
    if (!strchr(line, '|')) {
        // No pipe - single command
        cmds[0].argc = 0;
        char *tok = strtok(line, " \t");
        while (tok && cmds[0].argc < 63) {
            cmds[0].argv[cmds[0].argc++] = strdup(tok);
            tok = strtok(NULL, " \t");
        }
        cmds[0].argv[cmds[0].argc] = NULL;
        *num_cmds = (cmds[0].argc > 0) ? 1 : 0;
        return *num_cmds;
    }
    
    // Parse pipeline - split by pipe first
    char *saveptr1;
    char *cmd_str = strtok_r(line, "|", &saveptr1);
    
    while (cmd_str && *num_cmds < 10) {
        // Trim leading spaces
        while (*cmd_str == ' ' || *cmd_str == '\t') cmd_str++;
        
        // Trim trailing spaces
        char *end = cmd_str + strlen(cmd_str) - 1;
        while (end > cmd_str && (*end == ' ' || *end == '\t')) {
            *end = '\0';
            end--;
        }
        
        if (strlen(cmd_str) == 0) {
            cmd_str = strtok_r(NULL, "|", &saveptr1);
            continue;
        }
        
        cmds[*num_cmds].argc = 0;
        
        // Parse this command's arguments using a separate copy
        char *cmd_copy = strdup(cmd_str);
        char *saveptr2;
        char *tok = strtok_r(cmd_copy, " \t", &saveptr2);
        
        while (tok && cmds[*num_cmds].argc < 63) {
            cmds[*num_cmds].argv[cmds[*num_cmds].argc++] = strdup(tok);
            tok = strtok_r(NULL, " \t", &saveptr2);
        }
        cmds[*num_cmds].argv[cmds[*num_cmds].argc] = NULL;
        free(cmd_copy);
        
        if (cmds[*num_cmds].argc > 0) {
            (*num_cmds)++;
        }
        
        cmd_str = strtok_r(NULL, "|", &saveptr1);
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
    
    pid_t pids[10];
    
    for (int i = 0; i < num_cmds; i++) {
        pid_t pid = fork();
        pids[i] = pid;
        
        if (pid == 0) {
            // Not first command: read from previous pipe
            if (i > 0) {
                if (dup2(pipefds[(i - 1) * 2], STDIN_FILENO) < 0) {
                    perror("dup2 stdin");
                    exit(1);
                }
            }
            
            // Not last command: write to next pipe
            if (i < num_cmds - 1) {
                if (dup2(pipefds[i * 2 + 1], STDOUT_FILENO) < 0) {
                    perror("dup2 stdout");
                    exit(1);
                }
            }
            
            // Close all pipe fds in child
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
    
    // Wait for all children in order
    for (int i = 0; i < num_cmds; i++) {
        int status;
        waitpid(pids[i], &status, 0);
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
        
        // Free strdup'd strings from parsing
        for (int i = 0; i < num_cmds; i++) {
            for (int j = 0; j < cmds[i].argc; j++) {
                free(cmds[i].argv[j]);
            }
        }
        
        free(line_copy);
    }
    
    free(line);
    free(fs);
    return 0;
}