/* include/fat_fs.h */
#ifndef FAT_FS_H
#define FAT_FS_H

#include "common.h"

/* FAT File System Configuration */
#define BLOCK_SIZE 512
#define MAX_BLOCKS 1024
#define MAX_FILENAME 255
#define FAT_EOC 0xFFFF  // End of chain marker
#define FAT_FREE 0x0000 // Free block marker

/* FAT Data Structures */
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
    dir_entry dir_entries[256];       // Directory entries
    uint8_t blocks[MAX_BLOCKS][BLOCK_SIZE];  // Data blocks
    uint32_t num_entries;
    uint32_t current_dir;  // Current directory entry index
} fat_fs;

extern fat_fs *fs;

/* Core FAT functions */
void fat_init(void);
int fat_save_image(const char *filename);
int fat_load_image(const char *filename);
uint16_t fat_alloc_block(void);
void fat_free_chain(uint16_t start_block);
uint32_t fat_find_entry(const char *name, uint32_t parent);
uint32_t fat_resolve_path(const char *path);
int fat_write_file(uint32_t entry_idx, const char *data, size_t size);
char* fat_read_file(uint32_t entry_idx);

/* FAT operations */
int fat_mkdir(const char *path);
int fat_touch(const char *path);
int fat_cd(const char *path);
void fat_pwd(void);
void fat_ls(const char *path);
void fat_cat(const char *path);
void fat_grep(const char *pattern, const char *filename);
void fat_sync_from_real_file(const char *path);

#endif /* FAT_FS_H */