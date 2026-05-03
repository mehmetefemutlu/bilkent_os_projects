#define FUSE_USE_VERSION 31

#include "mfs.h"

#include <errno.h>
#include <fcntl.h>
#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fd_disk = -1;
static struct mfs_superblock superblock;

int mfs_read_block(int fd, void *block, uint32_t block_no)
{
    off_t offset = (off_t) block_no * MFS_BLOCK_SIZE;

    if (lseek(fd, offset, SEEK_SET) == (off_t) -1) {
        return -1;
    }

    return read(fd, block, MFS_BLOCK_SIZE) == MFS_BLOCK_SIZE ? 0 : -1;
}

int mfs_write_block(int fd, const void *block, uint32_t block_no)
{
    off_t offset = (off_t) block_no * MFS_BLOCK_SIZE;

    if (lseek(fd, offset, SEEK_SET) == (off_t) -1) {
        return -1;
    }

    return write(fd, block, MFS_BLOCK_SIZE) == MFS_BLOCK_SIZE ? 0 : -1;
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

static const char *path_name(const char *path)
{
    if (strcmp(path, "/") == 0) {
        return "";
    }
    if (path[0] != '/' || strchr(path + 1, '/') != NULL) {
        return NULL;
    }
    return path + 1;
}

static int read_inode(uint32_t inode_no, struct mfs_inode *inode)
{
    uint8_t block[MFS_BLOCK_SIZE];
    uint32_t index;
    uint32_t block_no;
    uint32_t block_index;

    if (inode_no < 1 || inode_no > MFS_INODE_COUNT) {
        return -ENOENT;
    }

    index = inode_no - 1;
    block_no = MFS_INODE_TABLE_START + index / 128;
    block_index = index % 128;

    if (mfs_read_block(fd_disk, block, block_no) != 0) {
        return -EIO;
    }

    *inode = ((struct mfs_inode *) block)[block_index];
    if (inode->inode_no == 0) {
        return -ENOENT;
    }

    return 0;
}

static int write_inode(const struct mfs_inode *inode)
{
    uint8_t block[MFS_BLOCK_SIZE];
    uint32_t index = inode->inode_no - 1;
    uint32_t block_no = MFS_INODE_TABLE_START + index / 128;
    uint32_t block_index = index % 128;

    if (inode->inode_no < 1 || inode->inode_no > MFS_INODE_COUNT) {
        return -EINVAL;
    }
    if (mfs_read_block(fd_disk, block, block_no) != 0) {
        return -EIO;
    }

    ((struct mfs_inode *) block)[block_index] = *inode;
    return mfs_write_block(fd_disk, block, block_no) == 0 ? 0 : -EIO;
}

static int read_root_dir(struct mfs_dir_entry entries[MFS_DIR_ENTRY_COUNT])
{
    return mfs_read_block(fd_disk, entries, MFS_ROOT_DIR_BLOCK) == 0 ? 0 : -EIO;
}

static int write_root_dir(const struct mfs_dir_entry entries[MFS_DIR_ENTRY_COUNT])
{
    return mfs_write_block(fd_disk, entries, MFS_ROOT_DIR_BLOCK) == 0 ? 0 : -EIO;
}

static int find_dir_entry(const char *name, struct mfs_dir_entry *entry, int *entry_index)
{
    struct mfs_dir_entry entries[MFS_DIR_ENTRY_COUNT];
    int rc = read_root_dir(entries);

    if (rc != 0) {
        return rc;
    }

    for (int i = 0; i < MFS_DIR_ENTRY_COUNT; i++) {
        if (entries[i].used && strcmp(entries[i].name, name) == 0) {
            if (entry != NULL) {
                *entry = entries[i];
            }
            if (entry_index != NULL) {
                *entry_index = i;
            }
            return 0;
        }
    }

    return -ENOENT;
}

static int allocate_inode(uint32_t *inode_no)
{
    uint8_t bitmap[MFS_BLOCK_SIZE];

    if (mfs_read_block(fd_disk, bitmap, MFS_INODE_BITMAP_BLOCK) != 0) {
        return -EIO;
    }

    for (uint32_t i = 1; i < MFS_INODE_COUNT; i++) {
        if (!mfs_get_bit(bitmap, i)) {
            mfs_set_bit(bitmap, i, 1);
            if (mfs_write_block(fd_disk, bitmap, MFS_INODE_BITMAP_BLOCK) != 0) {
                return -EIO;
            }
            *inode_no = i + 1;
            return 0;
        }
    }

    return -ENOSPC;
}

static int free_inode(uint32_t inode_no)
{
    uint8_t bitmap[MFS_BLOCK_SIZE];

    if (inode_no <= MFS_ROOT_INODE || inode_no > MFS_INODE_COUNT) {
        return -EINVAL;
    }
    if (mfs_read_block(fd_disk, bitmap, MFS_INODE_BITMAP_BLOCK) != 0) {
        return -EIO;
    }

    mfs_set_bit(bitmap, inode_no - 1, 0);
    return mfs_write_block(fd_disk, bitmap, MFS_INODE_BITMAP_BLOCK) == 0 ? 0 : -EIO;
}

static int mfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    const char *name;
    struct mfs_dir_entry entry;
    struct mfs_inode inode;
    int rc;

    (void) fi;
    memset(stbuf, 0, sizeof(*stbuf));

    name = path_name(path);
    if (name == NULL) {
        return -ENOENT;
    }

    if (name[0] == '\0') {
        rc = read_inode(MFS_ROOT_INODE, &inode);
    } else {
        rc = find_dir_entry(name, &entry, NULL);
        if (rc != 0) {
            return rc;
        }
        rc = read_inode(entry.inode_no, &inode);
    }

    if (rc != 0) {
        return rc;
    }

    stbuf->st_ino = inode.inode_no;
    stbuf->st_mode = inode.mode;
    stbuf->st_nlink = S_ISDIR(inode.mode) ? 2 : 1;
    stbuf->st_size = inode.size;
    stbuf->st_blocks = inode.block_count;
    stbuf->st_ctime = (time_t) inode.ctime;
    stbuf->st_mtime = (time_t) inode.mtime;
    stbuf->st_atime = (time_t) inode.atime;
    return 0;
}

static int mfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags)
{
    struct mfs_dir_entry entries[MFS_DIR_ENTRY_COUNT];
    int rc;

    (void) offset;
    (void) fi;
    (void) flags;

    if (strcmp(path, "/") != 0) {
        return -ENOENT;
    }

    rc = read_root_dir(entries);
    if (rc != 0) {
        return rc;
    }

    for (int i = 0; i < MFS_DIR_ENTRY_COUNT; i++) {
        if (entries[i].used) {
            filler(buf, entries[i].name, NULL, 0, 0);
        }
    }

    return 0;
}

static int mfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    const char *name = path_name(path);
    struct mfs_dir_entry entries[MFS_DIR_ENTRY_COUNT];
    struct mfs_inode inode;
    uint32_t inode_no;
    int free_slot = -1;
    int rc;
    time_t now;

    (void) fi;

    if (name == NULL || name[0] == '\0') {
        return -EINVAL;
    }
    if (strlen(name) > MFS_MAX_FILENAME) {
        return -ENAMETOOLONG;
    }
    if (find_dir_entry(name, NULL, NULL) == 0) {
        return -EEXIST;
    }

    rc = read_root_dir(entries);
    if (rc != 0) {
        return rc;
    }
    for (int i = 2; i < MFS_DIR_ENTRY_COUNT; i++) {
        if (!entries[i].used) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) {
        return -ENOSPC;
    }

    rc = allocate_inode(&inode_no);
    if (rc != 0) {
        return rc;
    }

    now = time(NULL);
    memset(&inode, 0, sizeof(inode));
    inode.inode_no = inode_no;
    inode.mode = S_IFREG | (mode & 0777);
    inode.ctime = (uint64_t) now;
    inode.mtime = (uint64_t) now;
    inode.atime = (uint64_t) now;
    rc = write_inode(&inode);
    if (rc != 0) {
        free_inode(inode_no);
        return rc;
    }

    memset(&entries[free_slot], 0, sizeof(entries[free_slot]));
    entries[free_slot].used = 1;
    entries[free_slot].inode_no = inode_no;
    strncpy(entries[free_slot].name, name, MFS_FILENAME_SIZE - 1);
    return write_root_dir(entries);
}

static int mfs_open(const char *path, struct fuse_file_info *fi)
{
    const char *name = path_name(path);
    struct mfs_dir_entry entry;

    (void) fi;

    if (name == NULL || name[0] == '\0') {
        return -EINVAL;
    }

    return find_dir_entry(name, &entry, NULL);
}

static int mfs_unlink(const char *path)
{
    const char *name = path_name(path);
    struct mfs_dir_entry entries[MFS_DIR_ENTRY_COUNT];
    struct mfs_inode empty_inode;
    int entry_index;
    int rc;
    uint32_t inode_no;

    if (name == NULL || name[0] == '\0') {
        return -EINVAL;
    }

    rc = find_dir_entry(name, NULL, &entry_index);
    if (rc != 0) {
        return rc;
    }

    rc = read_root_dir(entries);
    if (rc != 0) {
        return rc;
    }

    inode_no = entries[entry_index].inode_no;
    memset(&entries[entry_index], 0, sizeof(entries[entry_index]));
    rc = write_root_dir(entries);
    if (rc != 0) {
        return rc;
    }

    memset(&empty_inode, 0, sizeof(empty_inode));
    empty_inode.inode_no = inode_no;
    rc = write_inode(&empty_inode);
    if (rc != 0) {
        return rc;
    }

    return free_inode(inode_no);
}

static int mfs_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    (void) path;
    (void) buf;
    (void) size;
    (void) offset;
    (void) fi;
    return 0;
}

static int mfs_write(const char *path, const char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
    (void) path;
    (void) buf;
    (void) size;
    (void) offset;
    (void) fi;
    return -EOPNOTSUPP;
}

static int mfs_release(const char *path, struct fuse_file_info *fi)
{
    (void) path;
    (void) fi;
    return 0;
}

static struct fuse_operations mfs_oper = {
    .getattr = mfs_getattr,
    .readdir = mfs_readdir,
    .create = mfs_create,
    .open = mfs_open,
    .read = mfs_read,
    .write = mfs_write,
    .unlink = mfs_unlink,
    .release = mfs_release,
};

static int load_superblock(void)
{
    uint8_t block[MFS_BLOCK_SIZE];

    if (mfs_read_block(fd_disk, block, MFS_SUPER_BLOCK) != 0) {
        return -1;
    }

    memcpy(&superblock, block, sizeof(superblock));
    if (superblock.magic != MFS_MAGIC ||
        superblock.block_size != MFS_BLOCK_SIZE ||
        superblock.inode_count != MFS_INODE_COUNT) {
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    char *disk_name;

    if (argc < 3) {
        fprintf(stderr, "usage: %s mountpoint diskfile [FUSE options]\n", argv[0]);
        return 1;
    }

    disk_name = argv[2];
    fd_disk = open(disk_name, O_RDWR);
    if (fd_disk < 0) {
        perror("open disk");
        return 1;
    }
    if (load_superblock() != 0) {
        fprintf(stderr, "invalid or unformatted MFS disk\n");
        close(fd_disk);
        return 1;
    }

    for (int i = 2; i < argc - 1; i++) {
        argv[i] = argv[i + 1];
    }
    argc--;

    int rc = fuse_main(argc, argv, &mfs_oper, NULL);
    close(fd_disk);
    return rc;
}
