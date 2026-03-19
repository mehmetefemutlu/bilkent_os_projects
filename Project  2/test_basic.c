#include <stdio.h>
#include "tus.h"

static void *worker_a(void *arg) {
    int loops = *(int *)arg;

    for (int i = 0; i < loops; i++) {
        printf("worker_a tid=%d iteration=%d\n", tus_gettid(), i);
        tus_yield(TUS_ANY);
    }

    printf("worker_a tid=%d done\n", tus_gettid());
    return NULL;
}

static void *worker_b(void *arg) {
    int loops = *(int *)arg;

    for (int i = 0; i < loops; i++) {
        printf("worker_b tid=%d iteration=%d\n", tus_gettid(), i);
        tus_yield(TUS_ANY);
    }

    printf("worker_b tid=%d done\n", tus_gettid());
    return NULL;
}

int main(void) {
    int loops_a = 3;
    int loops_b = 4;
    int main_tid;
    int tid_a;
    int tid_b;

    main_tid = tus_init(ALG_FCFS);
    if (main_tid == TUS_ERROR) {
        fprintf(stderr, "tus_init failed\n");
        return 1;
    }

    tid_a = tus_create_thread(worker_a, &loops_a);
    tid_b = tus_create_thread(worker_b, &loops_b);
    if (tid_a == TUS_ERROR || tid_b == TUS_ERROR) {
        fprintf(stderr, "thread creation failed\n");
        return 1;
    }

    printf("main tid=%d created threads %d and %d\n", main_tid, tid_a, tid_b);

    while (tus_join(tid_a) != tid_a) {
        printf("main join on %d failed\n", tid_a);
        return 1;
    }

    while (tus_join(tid_b) != tid_b) {
        printf("main join on %d failed\n", tid_b);
        return 1;
    }

    printf("basic test completed\n");
    return 0;
}
