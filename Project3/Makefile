CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pedantic -g -pthread

all: rsm.o

rsm.o: rsm.c rsm.h
	$(CC) $(CFLAGS) -c rsm.c

clean:
	rm -f rsm.o
