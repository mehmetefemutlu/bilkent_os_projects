CC = gcc
CFLAGS = -O2 -Wall -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=31

all: mfs make_mfs

mfs: mfs.c mfs.h
	$(CC) $(CFLAGS) -c -o mfs.o mfs.c
	$(CC) mfs.o -lfuse3 $(CFLAGS) -o mfs

make_mfs: make_mfs.c mfs.h
	$(CC) -Wall -g -D_FILE_OFFSET_BITS=64 make_mfs.c -o make_mfs

clean:
	rm -f *~ *.o mfs make_mfs
