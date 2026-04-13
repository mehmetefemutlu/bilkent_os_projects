/*
 * myapp.c – Demonstration application for the RSM library
 *
 * CS342 Operating Systems – Project 3
 *
 * Usage:  ./myapp <0|1>
 *   0 – avoidance DISABLED  → produces a deadlock; detection reports it
 *   1 – avoidance ENABLED   → Banker's algorithm prevents the deadlock
 *
 * Scenario
 * --------
 *   3 processes  (P0, P1, P2)
 *   5 resource types (R0 … R4), each with exactly 1 instance
 *   exist[] = {1, 1, 1, 1, 1}
 *
 *   P0 : grabs R0  →  sleeps 2 s  →  tries R1
 *   P1 : sleeps 1 s  →  grabs R1  →  sleeps 1 s  →  tries R0
 *   P2 : grabs R2  →  sleeps 1 s  →  grabs R3  →  sleeps 1 s  →  releases
 *
 *   With flag=0:
 *     P0 holds R0 and blocks waiting for R1 (held by P1).
 *     P1 holds R1 and blocks waiting for R0 (held by P0).
 *     Classic two-process circular wait → rsm_detection() returns 2.
 *
 *   With flag=1 (max demands below):
 *     P0 max = {1,1,0,0,0},  P1 max = {1,1,0,0,0},  P2 max = {0,0,1,1,0}
 *     Banker's algorithm finds that granting R1 to P1 while P0 holds R0
 *     leads to an unsafe state and therefore denies P1 until P0 finishes.
 *     All processes complete without deadlock.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "rsm.h"

#define NUMP 3   // number of child processes
#define NUMR 5   // number of resource types

/* Set by main from argv[1] */
static int AVOID = 0;

/* ------------------------------------------------------------------ */
/* Utility: pretty-print a resource vector                            */
/* ------------------------------------------------------------------ */
static void pr(int apid, const char *msg, const int r[])
{
    int i;
    printf("[P%d] %s [", apid, msg);
    for (i = 0; i < NUMR; ++i)
        printf(i < NUMR - 1 ? "%d," : "%d", r[i]);
    printf("]\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/*  P0: grab R0, wait 2 s, then request R1                            */
/*      With avoid=1 this is always granted first → P0 finishes →    */
/*      releases R0 and R1 → unblocks P1.                             */
/* ------------------------------------------------------------------ */
static void func_p0(int apid)
{
    int req[MAX_RT], rel[MAX_RT];
    memset(req, 0, sizeof req);
    memset(rel, 0, sizeof rel);

    rsm_process_started(apid);

    if (AVOID) {
        int claim[MAX_RT];
        memset(claim, 0, sizeof claim);
        claim[0] = 1; claim[1] = 1;          /* max: R0=1, R1=1 */
        rsm_claim(claim);
    }

    /* Request R0 */
    req[0] = 1;
    pr(apid, "requesting", req);
    rsm_request(req);
    pr(apid, "acquired  ", req);

    sleep(2);

    /* Request R1 */
    memset(req, 0, sizeof req);
    req[1] = 1;
    pr(apid, "requesting", req);
    rsm_request(req);
    pr(apid, "acquired  ", req);

    sleep(1);

    /* Release R0, then R1 */
    memset(rel, 0, sizeof rel);
    rel[0] = 1;
    rsm_release(rel);

    memset(rel, 0, sizeof rel);
    rel[1] = 1;
    rsm_release(rel);

    printf("[P%d] done\n", apid);
    fflush(stdout);
    rsm_process_ended();
    exit(0);
}

/* ------------------------------------------------------------------ */
/*  P1: sleep 1 s, grab R1, wait 1 s, then request R0                */
/*      With avoid=0: deadlocks with P0.                              */
/*      With avoid=1: Banker's blocks P1 when unsafe; P1 proceeds     */
/*                    only after P0 has released both resources.      */
/* ------------------------------------------------------------------ */
static void func_p1(int apid)
{
    int req[MAX_RT], rel[MAX_RT];
    memset(req, 0, sizeof req);
    memset(rel, 0, sizeof rel);

    rsm_process_started(apid);

    if (AVOID) {
        int claim[MAX_RT];
        memset(claim, 0, sizeof claim);
        claim[0] = 1; claim[1] = 1;          /* max: R0=1, R1=1 */
        rsm_claim(claim);
    }

    sleep(1);   /* let P0 grab R0 first */

    /* Request R1 */
    req[1] = 1;
    pr(apid, "requesting", req);
    rsm_request(req);
    pr(apid, "acquired  ", req);

    sleep(1);

    /* Request R0  ← this creates the circular wait in the no-avoid case */
    memset(req, 0, sizeof req);
    req[0] = 1;
    pr(apid, "requesting", req);
    rsm_request(req);
    pr(apid, "acquired  ", req);

    sleep(1);

    /* Release R1, then R0 */
    memset(rel, 0, sizeof rel);
    rel[1] = 1;
    rsm_release(rel);

    memset(rel, 0, sizeof rel);
    rel[0] = 1;
    rsm_release(rel);

    printf("[P%d] done\n", apid);
    fflush(stdout);
    rsm_process_ended();
    exit(0);
}

/* ------------------------------------------------------------------ */
/*  P2: grab R2, then R3, release both – always completes cleanly.   */
/* ------------------------------------------------------------------ */
static void func_p2(int apid)
{
    int req[MAX_RT], rel[MAX_RT];
    memset(req, 0, sizeof req);
    memset(rel, 0, sizeof rel);

    rsm_process_started(apid);

    if (AVOID) {
        int claim[MAX_RT];
        memset(claim, 0, sizeof claim);
        claim[2] = 1; claim[3] = 1;          /* max: R2=1, R3=1 */
        rsm_claim(claim);
    }

    /* Request R2 */
    req[2] = 1;
    pr(apid, "requesting", req);
    rsm_request(req);
    pr(apid, "acquired  ", req);

    sleep(1);

    /* Request R3 */
    memset(req, 0, sizeof req);
    req[3] = 1;
    pr(apid, "requesting", req);
    rsm_request(req);
    pr(apid, "acquired  ", req);

    sleep(1);

    /* Release R2 and R3 together */
    rel[2] = 1; rel[3] = 1;
    rsm_release(rel);

    printf("[P%d] done\n", apid);
    fflush(stdout);
    rsm_process_ended();
    exit(0);
}

/* ================================================================== */
int main(int argc, char **argv)
{
    int    exist[MAX_RT];
    pid_t  pids[NUMP];
    int    i, ret;
    int    deadlock_reported = 0;
    char  *endptr = NULL;
    long   parsed_flag;

    if (argc != 2) {
        fprintf(stderr,
                "Usage: %s <flag>\n"
                "  flag=0  avoidance disabled  (deadlock occurs & is detected)\n"
                "  flag=1  avoidance enabled   (deadlock is prevented)\n",
                argv[0]);
        return 1;
    }
    parsed_flag = strtol(argv[1], &endptr, 10);
    if (argv[1][0] == '\0' || endptr == argv[1] || *endptr != '\0' ||
        (parsed_flag != 0 && parsed_flag != 1)) {
        fprintf(stderr,
                "Usage: %s <flag>\n"
                "  flag=0  avoidance disabled  (deadlock occurs & is detected)\n"
                "  flag=1  avoidance enabled   (deadlock is prevented)\n",
                argv[0]);
        return 1;
    }
    AVOID = (int)parsed_flag;

    /* ---- Initialise library ---- */
    memset(exist, 0, sizeof exist);
    for (i = 0; i < NUMR; ++i) exist[i] = 1;  /* 1 instance per type */

    if (rsm_init(NUMP, NUMR, exist, AVOID) != 0) {
        fprintf(stderr, "rsm_init failed – is /dev/shm/%s already open?\n"
                        "Try: rm -f /dev/shm/cs342_rsm_shm\n",
                "cs342_rsm_shm");
        return 1;
    }

    printf("=== CS342 Project 3 – %s ===\n",
           AVOID ? "Deadlock Avoidance (flag=1)" : "Deadlock Detection (flag=0)");
    printf("Resources: %d types, 1 instance each\n\n", NUMR);
    fflush(stdout);

    /* ---- Fork child processes ---- */
    pids[0] = fork();
    if (pids[0] == 0) func_p0(0);

    pids[1] = fork();
    if (pids[1] == 0) func_p1(1);

    pids[2] = fork();
    if (pids[2] == 0) func_p2(2);

    /* ---- Parent: monitor loop ---- */
    for (i = 0; i < 14; ++i) {
        sleep(1);

        rsm_print_state("Current state");

        ret = rsm_detection();
        if (ret < 0) {
            fprintf(stderr, "rsm_detection error\n");
        } else if (ret > 0 && !deadlock_reported) {
            printf("\n*** DEADLOCK DETECTED: %d process(es) deadlocked ***\n\n",
                   ret);
            fflush(stdout);
            deadlock_reported = 1;

            /*
             * In the no-avoidance scenario the deadlocked children will
             * never wake; kill them so the parent can clean up.
             */
            if (!AVOID) {
                printf("Terminating deadlocked processes...\n");
                fflush(stdout);
                for (i = 0; i < NUMP; ++i)
                    kill(pids[i], SIGKILL);
                break;
            }
        } else if (AVOID && ret == 0) {
            /* Check whether all children have already finished */
            int all_done = 1;
            int k;
            for (k = 0; k < NUMP; ++k) {
                if (waitpid(pids[k], NULL, WNOHANG) == 0) {
                    all_done = 0;
                }
            }
            if (all_done) {
                printf("\nAll processes finished without deadlock.\n");
                fflush(stdout);
                break;
            }
        }
    }

    /* ---- Reap children ---- */
    for (i = 0; i < NUMP; ++i)
        waitpid(pids[i], NULL, 0);

    rsm_destroy();

    if (AVOID)
        printf("\n=== Avoidance succeeded: no deadlock occurred. ===\n");
    else
        printf("\n=== Detection complete. ===\n");

    return 0;
}
