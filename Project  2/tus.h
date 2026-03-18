#ifndef TUS_H
#define TUS_H

#include <ucontext.h>

#define TUS_MAXTHREADS 64
#define TUS_STACKSIZE 16384

#define TUS_SUCCESS 0
#define TUS_ERROR   -1
#define TUS_ANY     0

#define ALG_FCFS    1
#define ALG_RANDOM  2

int tus_init(int salg);
int tus_create_thread(void *(*tsf)(void *), void *targ);
int tus_yield(int tid);
void tus_exit(void);
int tus_join(int tid);
int tus_cancel(int tid);
int tus_gettid(void);

#endif