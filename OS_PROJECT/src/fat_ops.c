/* src/fat_ops.c */
#include "../include/fat_fs.h"

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
    
    free(copy);
    return 0;
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