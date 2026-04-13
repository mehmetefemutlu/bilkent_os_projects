/*
 * myapp_scaled.c – Scalable experiment application for the RSM library
 *
 * Usage:
 *   ./myapp_scaled <0|1> <num_resource_types>
 *
 *     0 – avoidance DISABLED
 *     1 – avoidance ENABLED
 *
 * Example:
 *   ./myapp_scaled 1 8
 *   ./myapp_scaled 1 16
 *   ./myapp_scaled 1 32
 *
 * Idea
 * ----
 * We do not keep a fixed hard-coded request pattern anymore.
 * Instead, when the number of resource types grows, each process:
 *   - claims more resources,
 *   - requests more resources,
 *   - and does so with overlapping ranges to preserve contention.
 *
 * This makes total execution time vs. number of resource types meaningful.
 *
 * Notes
 * -----
 * - For timing experiments, use flag=1 (avoidance enabled), so all processes finish.
 * - With flag=0, due to opposing request orders and overlapping claims, deadlock may occur;
 *   the parent detects it and terminates the children.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

#include "rsm.h"

#define NUMP 3               /* number of child processes */
#define MAX_LOCAL_RT MAX_RT  /* convenience alias         */

/* Set by main from argv */
static int AVOID = 0;
static int G_NUMR = 0;       /* active number of resource types */

/* ------------------------------------------------------------------ */
/* Utility: pretty-print a resource vector                            */
/* ------------------------------------------------------------------ */
static void pr(int apid, const char *msg, const int r[])
{
    int i;
    printf("[P%d] %s [", apid, msg);
    for (i = 0; i < G_NUMR; ++i)
        printf(i < G_NUMR - 1 ? "%d," : "%d", r[i]);
    printf("]\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Build a claim vector: claim 'span' resources starting from 'start' */
/* wrapping around modulo G_NUMR                                      */
/* ------------------------------------------------------------------ */
static void build_claim_range(int claim[], int start, int span)
{
    int i;
    memset(claim, 0, sizeof(int) * MAX_LOCAL_RT);

    for (i = 0; i < span; ++i) {
        int idx = (start + i) % G_NUMR;
        claim[idx] = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Build an ordered list of resource indices from a claim vector      */
/* ------------------------------------------------------------------ */
static int build_order_from_claim(const int claim[], int order[])
{
    int i, cnt = 0;
    for (i = 0; i < G_NUMR; ++i) {
        if (claim[i]) order[cnt++] = i;
    }
    return cnt;
}

/* ------------------------------------------------------------------ */
/* Reverse an integer array in place                                  */
/* ------------------------------------------------------------------ */
static void reverse_order(int arr[], int n)
{
    int i;
    for (i = 0; i < n / 2; ++i) {
        int tmp = arr[i];
        arr[i] = arr[n - 1 - i];
        arr[n - 1 - i] = tmp;
    }
}

/* ------------------------------------------------------------------ */
/* Rotate an integer array left by k positions                        */
/* ------------------------------------------------------------------ */
static void rotate_left(int arr[], int n, int k)
{
    int tmp[MAX_LOCAL_RT];
    int i;

    if (n <= 0) return;
    k %= n;
    if (k < 0) k += n;

    for (i = 0; i < n; ++i)
        tmp[i] = arr[(i + k) % n];

    for (i = 0; i < n; ++i)
        arr[i] = tmp[i];
}

/* ------------------------------------------------------------------ */
/* Generic scalable process logic                                     */
/* mode = 0 -> forward order                                          */
/* mode = 1 -> reverse order                                          */
/* mode = 2 -> rotated order                                          */
/* ------------------------------------------------------------------ */
static void run_process(int apid, int start, int span, int mode, int initial_sleep)
{
    int req[MAX_LOCAL_RT], rel[MAX_LOCAL_RT], claim[MAX_LOCAL_RT];
    int order[MAX_LOCAL_RT];
    int count, i;

    memset(req, 0, sizeof req);
    memset(rel, 0, sizeof rel);
    memset(claim, 0, sizeof claim);

    rsm_process_started(apid);

    build_claim_range(claim, start, span);

    if (AVOID) {
        pr(apid, "claiming   ", claim);
        rsm_claim(claim);
    }

    count = build_order_from_claim(claim, order);

    if (mode == 1) {
        reverse_order(order, count);
    } else if (mode == 2) {
        rotate_left(order, count, count / 2);
    }

    if (initial_sleep > 0)
        sleep(initial_sleep);

    /* Request claimed resources one by one */
    for (i = 0; i < count; ++i) {
        memset(req, 0, sizeof req);
        req[order[i]] = 1;
        pr(apid, "requesting", req);
        rsm_request(req);
        pr(apid, "acquired  ", req);

        /*
         * Small pause to increase overlap/interleaving.
         * usleep keeps the experiment faster than full 1-2 second sleeps.
         */
        usleep(100000);  /* 100 ms */
    }

    /* Simulate some work while holding the resources */
    usleep(200000);      /* 200 ms */

    /* Release everything in the same order */
    for (i = 0; i < count; ++i) {
        memset(rel, 0, sizeof rel);
        rel[order[i]] = 1;
        pr(apid, "releasing ", rel);
        rsm_release(rel);
        usleep(50000);   /* 50 ms */
    }

    printf("[P%d] done\n", apid);
    fflush(stdout);
    rsm_process_ended();
    exit(0);
}

/* ------------------------------------------------------------------ */
/* Wrappers for the three processes                                   */
/* Each process claims about half of all resources, with overlap.     */
/* ------------------------------------------------------------------ */
static void func_p0(int apid)
{
    int span  = G_NUMR / 2;
    if (span < 2) span = 2;

    run_process(apid, 0, span, 0, 0);
}

static void func_p1(int apid)
{
    int span  = G_NUMR / 2;
    int start = G_NUMR / 4;
    if (span < 2) span = 2;

    run_process(apid, start, span, 1, 0);
}

static void func_p2(int apid)
{
    int span  = G_NUMR / 2;
    int start = G_NUMR / 2;
    if (span < 2) span = 2;

    run_process(apid, start, span, 2, 0);
}

/* ------------------------------------------------------------------ */
/* Compute elapsed time in seconds                                    */
/* ------------------------------------------------------------------ */
static double elapsed_seconds(struct timespec a, struct timespec b)
{
    double sec  = (double)(b.tv_sec  - a.tv_sec);
    double nsec = (double)(b.tv_nsec - a.tv_nsec) / 1e9;
    return sec + nsec;
}

/* ================================================================== */
int main(int argc, char **argv)
{
    int exist[MAX_LOCAL_RT];
    pid_t pids[NUMP];
    int i, ret;
    int deadlock_reported = 0;
    struct timespec t0, t1;

    if (argc != 3) {
        fprintf(stderr,
                "Usage: %s <flag> <num_resource_types>\n"
                "  flag=0  avoidance disabled\n"
                "  flag=1  avoidance enabled\n"
                "  num_resource_types must be in [4, %d]\n",
                argv[0], MAX_RT);
        return 1;
    }

    AVOID  = atoi(argv[1]);
    G_NUMR = atoi(argv[2]);

    if (G_NUMR < 4 || G_NUMR > MAX_RT) {
        fprintf(stderr, "num_resource_types must be between 4 and %d\n", MAX_RT);
        return 1;
    }

    /* ---- Initialise library ---- */
    memset(exist, 0, sizeof exist);
    for (i = 0; i < G_NUMR; ++i)
        exist[i] = 1;  /* 1 instance per type */

    if (rsm_init(NUMP, G_NUMR, exist, AVOID) != 0) {
        fprintf(stderr, "rsm_init failed – is /dev/shm/%s already open?\n"
                        "Try: rm -f /dev/shm/cs342_rsm_shm\n",
                "cs342_rsm_shm");
        return 1;
    }

    printf("=== Scalable RSM Experiment – %s ===\n",
           AVOID ? "Avoidance ON" : "Avoidance OFF");
    printf("Processes: %d\n", NUMP);
    printf("Resources: %d types, 1 instance each\n", G_NUMR);
    printf("Each process claims about %d resources with overlap.\n\n", G_NUMR / 2);
    fflush(stdout);

    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* ---- Fork child processes ---- */
    pids[0] = fork();
    if (pids[0] == 0) func_p0(0);

    pids[1] = fork();
    if (pids[1] == 0) func_p1(1);

    pids[2] = fork();
    if (pids[2] == 0) func_p2(2);

    /* ---- Parent: monitor loop ---- */
    while (1) {
        sleep(1);

        rsm_print_state("Current state");

        ret = rsm_detection();
        if (ret < 0) {
            fprintf(stderr, "rsm_detection error\n");
        } else if (ret > 0 && !deadlock_reported) {
            printf("\n*** DEADLOCK DETECTED: %d process(es) deadlocked ***\n\n", ret);
            fflush(stdout);
            deadlock_reported = 1;

            if (!AVOID) {
                printf("Terminating deadlocked processes...\n");
                fflush(stdout);
                for (i = 0; i < NUMP; ++i)
                    kill(pids[i], SIGKILL);
                break;
            }
        }

        /* Check whether all children have already finished */
        {
            int all_done = 1;
            for (i = 0; i < NUMP; ++i) {
                if (waitpid(pids[i], NULL, WNOHANG) == 0)
                    all_done = 0;
            }

            if (all_done) {
                if (AVOID)
                    printf("\nAll processes finished without deadlock.\n");
                break;
            }
        }

        /* If deadlock already happened in flag=0 and was reported, stop after kill */
        if (deadlock_reported && !AVOID)
            break;
    }

    /* ---- Reap any remaining children ---- */
    for (i = 0; i < NUMP; ++i)
        waitpid(pids[i], NULL, 0);

    clock_gettime(CLOCK_MONOTONIC, &t1);

    rsm_destroy();

    printf("\nTotal execution time: %.6f seconds\n", elapsed_seconds(t0, t1));

    if (AVOID)
        printf("=== Avoidance run completed. ===\n");
    else
        printf("=== Detection run completed. ===\n");

    return 0;
}
