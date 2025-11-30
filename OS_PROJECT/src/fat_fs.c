/* src/fat_fs.c */
#include "../include/fat_fs.h"

fat_fs *fs = NULL;

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