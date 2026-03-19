CS342- OPERATING SYSTEMS- PROJECT 2- THREAD SUPPORTING AT USER SPACE(x64)
=====================
Mehmet Efe Mutlu       -  22303326
Emir Said Bakan        -  22302852
Ahmet Eren Gökalp      -  22302136

Overview
--------
This project implements a small user-level cooperative threading library for
Linux x86-64 in C. Context switching is done with getcontext() and
setcontext(). The library supports two scheduling algorithms:

- FCFS
- RANDOM

Files:
---------
- tus.h           : public interface and constants
- tus.c           : threading library implementation
- test_basic.c    : basic cooperative scheduling / join test
- test_cancel.c   : cancel / join behavior test

Implemented API
---------------
The following functions are implemented in tus.c / tus.h:

- int tus_init(int salg)
- int tus_create_thread(void *(*tsf)(void *), void *targ)
- int tus_yield(int tid)
- void tus_exit(void)
- int tus_join(int tid)
- int tus_cancel(int tid)
- int tus_gettid(void)

Important Notes
---------------
- The main thread is assigned tid 1.
- Thread ids are reused after join so the implementation respects the maximum
  simultaneous thread limit.
- tus_join() waits cooperatively by repeatedly yielding until the target thread
  reaches the ended state.
- If the last non-ended thread exits, the process terminates.
