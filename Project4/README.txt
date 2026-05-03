CS342 Project 4 - MFS starter

Current status:
- make_mfs formats an existing disk file with the MFS superblock, block bitmap,
  inode bitmap, root inode, and root directory entries "." and "..".
- mfs mounts a formatted disk with FUSE3 and supports basic root-directory
  metadata operations: getattr, readdir, create, open, release, and unlink.
- read currently returns EOF and write returns EOPNOTSUPP. Indexed data block
  allocation and append support are intentionally left for the next phase.

Example:
dd bs=16K count=1K if=/dev/zero of=disk1
make
./make_mfs disk1
mkdir -p /tmp/fusemountpoint
./mfs /tmp/fusemountpoint disk1
fusermount -u /tmp/fusemountpoint
