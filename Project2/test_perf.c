#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "tus.h"

typedef struct {
    int iterations;
} worker_arg_t;

static void *perf_worker(void *arg) {
    worker_arg_t *warg = (worker_arg_t *)arg;

    for (int i = 0; i < warg->iterations; i++) {
        tus_yield(TUS_ANY);
    }

    return NULL;
}

static double elapsed_ms(struct timespec start, struct timespec end) {
    double sec = (double)(end.tv_sec - start.tv_sec);
    double nsec = (double)(end.tv_nsec - start.tv_nsec);
    return sec * 1000.0 + nsec / 1000000.0;
}

int main(int argc, char *argv[]) {
    int alg;
    int nthreads;
    int iterations;
    int tids[TUS_MAXTHREADS - 1];
    worker_arg_t arg;
    struct timespec start, end;

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <fcfs|random> <num_threads> <iterations>\n", argv[0]);
        fprintf(stderr, "Example: %s fcfs 8 10000\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "fcfs") == 0) {
        alg = ALG_FCFS;
    } else if (strcmp(argv[1], "random") == 0) {
        alg = ALG_RANDOM;
    } else {
        fprintf(stderr, "Invalid scheduler. Use fcfs or random.\n");
        return 1;
    }

    nthreads = atoi(argv[2]);
    iterations = atoi(argv[3]);

    if (nthreads < 1 || nthreads > TUS_MAXTHREADS - 1) {
        fprintf(stderr, "num_threads must be between 1 and %d\n", TUS_MAXTHREADS - 1);
        return 1;
    }

    if (iterations < 1) {
        fprintf(stderr, "iterations must be positive\n");
        return 1;
    }

    if (tus_init(alg) == TUS_ERROR) {
        fprintf(stderr, "tus_init failed\n");
        return 1;
    }

    arg.iterations = iterations;

    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < nthreads; i++) {
        tids[i] = tus_create_thread(perf_worker, &arg);
        if (tids[i] == TUS_ERROR) {
            fprintf(stderr, "tus_create_thread failed at thread %d\n", i);
            return 1;
        }
    }

    for (int i = 0; i < nthreads; i++) {
        if (tus_join(tids[i]) == TUS_ERROR) {
            fprintf(stderr, "tus_join failed for tid %d\n", tids[i]);
            return 1;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    printf("scheduler=%s threads=%d iterations=%d total_yields=%d elapsed_ms=%.3f\n",
           argv[1],
           nthreads,
           iterations,
           nthreads * iterations,
           elapsed_ms(start, end));

    return 0;
}
