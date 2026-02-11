all: findlwp

findlwp: findlwp.o
	gcc -Wall -O2 -g -Wextra -o findlwp findlwp.o

findlwp.o: findlwp.c
	gcc -Wall -O2 -c findlwp.c

clean:
	rm -f *.o findlwp
