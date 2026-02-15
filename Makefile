all: findlwp findlwt

findlwp: findlwp.o
	gcc -Wall -O2 -g -Wextra -o findlwp findlwp.o

findlwp.o: findlwp.c
	gcc -Wall -O2 -c findlwp.c

findlwt: findlwt.o
	gcc -Wall -O2 -g -Wextra -o findlwt findlwt.o

findlwt.o: findlwt.c
	gcc -Wall -O2 -g -Wextra -c findlwt.c

clean:
	rm -f *.o findlwp findlwt
