all: test_basic test_cancel test_perf

test_basic: tus.c tus.h test_basic.c
	gcc -Wall -Wextra -std=c11 -g tus.c test_basic.c -o test_basic

test_cancel: tus.c tus.h test_cancel.c
	gcc -Wall -Wextra -std=c11 -g tus.c test_cancel.c -o test_cancel

test_perf: tus.c tus.h test_perf.c
	gcc -Wall -Wextra -std=c11 -g tus.c test_perf.c -o test_perf

clean:
	rm -f test_basic test_cancel test_perf *.o
