#include "mfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int mfs_read_block(int fd_disk, void *block, uint32_t block_no)
{
    off_t offset = (off_t) block_no * MFS_BLOCK_SIZE;

    if (lseek(fd_disk, offset, SEEK_SET) == (off_t) -1) {
        return -1;
    }

    return read(fd_disk, block, MFS_BLOCK_SIZE) == MFS_BLOCK_SIZE ? 0 : -1;
}

int mfs_write_block(int fd_disk, const void *block, uint32_t block_no)
{
    off_t offset = (off_t) block_no * MFS_BLOCK_SIZE;

    if (lseek(fd_disk, offset, SEEK_SET) == (off_t) -1) {
        return -1;
    }

    return write(fd_disk, block, MFS_BLOCK_SIZE) == MFS_BLOCK_SIZE ? 0 : -1;
}

int mfs_get_bit(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit / 8] >> (bit % 8)) & 1u;
}

void mfs_set_bit(uint8_t *bitmap, uint32_t bit, int value)
{
    if (value) {
        bitmap[bit / 8] |= (uint8_t) (1u << (bit % 8));
    } else {
        bitmap[bit / 8] &= (uint8_t) ~(1u << (bit % 8));
    }
}

static uint32_t disk_block_count(int fd)
{
    struct stat st;

    if (fstat(fd, &st) != 0) {
        return 0;
    }

    return (uint32_t) (st.st_size / MFS_BLOCK_SIZE);
}

int main(int argc, char *argv[])
{
    uint8_t block[MFS_BLOCK_SIZE];
    uint32_t block_count;
    int fd;
    time_t now;

    if (argc != 2) {
        fprintf(stderr, "usage: %s diskfile\n", argv[0]);
        return 1;
    }

    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    block_count = disk_block_count(fd);
    if (block_count < 128 || block_count > 4096) {
        fprintf(stderr, "disk must be between 2 MB and 64 MB\n");
        close(fd);
        return 1;
    }

    memset(block, 0, sizeof(block));
    struct mfs_superblock *sb = (struct mfs_superblock *) block;
    sb->magic = MFS_MAGIC;
    sb->block_size = MFS_BLOCK_SIZE;
    sb->block_count = block_count;
    sb->inode_count = MFS_INODE_COUNT;
    sb->inode_table_start = MFS_INODE_TABLE_START;
    sb->root_dir_block = MFS_ROOT_DIR_BLOCK;
    sb->first_data_block = MFS_FIRST_DATA_BLOCK;
    sb->max_filename = MFS_MAX_FILENAME;
    if (mfs_write_block(fd, block, MFS_SUPER_BLOCK) != 0) {
        perror("write superblock");
        close(fd);
        return 1;
    }

    memset(block, 0, sizeof(block));
    for (uint32_t i = 0; i < MFS_FIRST_DATA_BLOCK; i++) {
        mfs_set_bit(block, i, 1);
    }
    if (mfs_write_block(fd, block, MFS_BLOCK_BITMAP_BLOCK) != 0) {
        perror("write block bitmap");
        close(fd);
        return 1;
    }

    memset(block, 0, sizeof(block));
    mfs_set_bit(block, MFS_ROOT_INODE - 1, 1);
    if (mfs_write_block(fd, block, MFS_INODE_BITMAP_BLOCK) != 0) {
        perror("write inode bitmap");
        close(fd);
        return 1;
    }

    now = time(NULL);
    memset(block, 0, sizeof(block));
    struct mfs_inode *inodes = (struct mfs_inode *) block;
    inodes[0].inode_no = MFS_ROOT_INODE;
    inodes[0].mode = S_IFDIR | 0755;
    inodes[0].size = 2 * sizeof(struct mfs_dir_entry);
    inodes[0].block_count = 1;
    inodes[0].index_block = MFS_ROOT_DIR_BLOCK;
    inodes[0].ctime = (uint64_t) now;
    inodes[0].mtime = (uint64_t) now;
    inodes[0].atime = (uint64_t) now;
    if (mfs_write_block(fd, block, MFS_INODE_TABLE_START) != 0) {
        perror("write inode table");
        close(fd);
        return 1;
    }
    memset(block, 0, sizeof(block));
    if (mfs_write_block(fd, block, MFS_INODE_TABLE_START + 1) != 0) {
        perror("write inode table");
        close(fd);
        return 1;
    }

    memset(block, 0, sizeof(block));
    struct mfs_dir_entry *entries = (struct mfs_dir_entry *) block;
    entries[0].inode_no = MFS_ROOT_INODE;
    entries[0].used = 1;
    strncpy(entries[0].name, ".", MFS_FILENAME_SIZE);
    entries[1].inode_no = MFS_ROOT_INODE;
    entries[1].used = 1;
    strncpy(entries[1].name, "..", MFS_FILENAME_SIZE);
    if (mfs_write_block(fd, block, MFS_ROOT_DIR_BLOCK) != 0) {
        perror("write root directory");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
