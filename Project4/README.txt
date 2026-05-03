CS342 Project 4 - MFS starter

Contributors:

Mehmet Efe Mutlu 22303326
Emir Said Bakan 2230
Ahmet Eren Gokalp 2230

Current status:
- make_mfs formats an existing disk file with the MFS superblock, block bitmap,
  inode bitmap, root inode, and root directory entries "." and "..".
- mfs mounts a formatted disk with FUSE3 and supports basic root-directory
  metadata operations: getattr, readdir, create, open, read, write, release,
  and unlink.
- File data is stored through an index block that maps logical file blocks to
  allocated data blocks on disk.
- read supports arbitrary offsets within a file.
- write supports append-only writes. A write is accepted only when the request
  offset matches the current file size; overwriting existing bytes is not
  supported.
- unlink frees the file's data blocks and index block in addition to removing
  the directory entry and inode.

Running Commands:
dd bs=16K count=1K if=/dev/zero of=disk1
make
./make_mfs disk1
mkdir -p /tmp/fusemountpoint
./mfs /tmp/fusemountpoint disk1
fusermount -u /tmp/fusemountpoint
