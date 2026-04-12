CC      = gcc
CFLAGS  = -Wall -Wextra -g
LDLIBS  = -lpthread -lrt

.PHONY: all clean

all: myapp

# ---- Object files ---------------------------------------------------
rsm.o: rsm.c rsm.h
	$(CC) $(CFLAGS) -c rsm.c

myapp.o: myapp.c rsm.h
	$(CC) $(CFLAGS) -c myapp.c

# ---- Static library -------------------------------------------------
librsm.a: rsm.o
	ar rcs librsm.a rsm.o

# ---- Demo application -----------------------------------------------
myapp: librsm.a myapp.o
	$(CC) $(CFLAGS) -o myapp myapp.o -L. -lrsm $(LDLIBS)

# ---- Clean ----------------------------------------------------------
clean:
	rm -f rsm.o myapp.o librsm.a myapp