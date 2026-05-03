#ifndef MFS_H
#define MFS_H

#include <stdint.h>
#include <sys/types.h>

#define MFS_MAGIC 0x4d465334u
#define MFS_BLOCK_SIZE 16384
#define MFS_MAX_FILENAME 31
#define MFS_FILENAME_SIZE 32
#define MFS_DIR_ENTRY_SIZE 64
#define MFS_INODE_SIZE 128
#define MFS_INODE_COUNT 256
#define MFS_DIR_ENTRY_COUNT 256
#define MFS_ROOT_INODE 1

#define MFS_SUPER_BLOCK 0
#define MFS_BLOCK_BITMAP_BLOCK 1
#define MFS_INODE_BITMAP_BLOCK 2
#define MFS_INODE_TABLE_START 3
#define MFS_INODE_TABLE_BLOCKS 2
#define MFS_ROOT_DIR_BLOCK 5
#define MFS_FIRST_DATA_BLOCK 6

struct mfs_superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t block_count;
    uint32_t inode_count;
    uint32_t inode_table_start;
    uint32_t root_dir_block;
    uint32_t first_data_block;
    uint32_t max_filename;
};

struct mfs_dir_entry {
    uint32_t inode_no;
    char name[MFS_FILENAME_SIZE];
    uint8_t used;
    uint8_t reserved[27];
};

struct mfs_inode {
    uint32_t inode_no;
    uint32_t mode;
    uint32_t size;
    uint32_t block_count;
    uint32_t index_block;
    uint64_t ctime;
    uint64_t mtime;
    uint64_t atime;
    uint8_t reserved[80];
};

typedef char mfs_inode_size_check[(sizeof(struct mfs_inode) == MFS_INODE_SIZE) ? 1 : -1];
typedef char mfs_dir_entry_size_check[(sizeof(struct mfs_dir_entry) == MFS_DIR_ENTRY_SIZE) ? 1 : -1];

int mfs_read_block(int fd_disk, void *block, uint32_t block_no);
int mfs_write_block(int fd_disk, const void *block, uint32_t block_no);
int mfs_get_bit(const uint8_t *bitmap, uint32_t bit);
void mfs_set_bit(uint8_t *bitmap, uint32_t bit, int value);

#endif
