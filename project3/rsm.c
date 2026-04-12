/*
 * rsm.c – Resource Management Library
 *
 * CS342 Operating Systems – Project 3
 *
 * Design summary
 * --------------
 *  - All shared state lives in a single POSIX shared-memory segment.
 *  - A single embedded sem_t (mutex, value 1) protects every field.
 *  - Each process has its own sem_t (process_sem, value 0) on which it
 *    blocks inside rsm_request when resources are unavailable or unsafe.
 *  - A barrier sem_t keeps the first rsm_request of any process waiting
 *    until every process has registered (called rsm_process_started /
 *    rsm_claim depending on the avoidance flag).
 *    Relay pattern: the process that opens the barrier does one sem_post;
 *    each waiting process does sem_wait then immediately sem_post, so the
 *    semaphore stays at 1 for all future callers.
 *  - Deadlock avoidance: Banker's algorithm (is_safe helper).
 *  - Deadlock detection: standard resource-allocation-graph reduction.
 *  - Only POSIX semaphores are used — no pthread mutexes or condvars.
 */

#include "rsm.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define RSM_SHM_NAME "/cs342_rsm_shm"

/* ====================================================================
 * Shared-memory layout
 * ==================================================================== */

typedef struct {
    int           initialized;
    int           avoid_enabled;
    int           process_count;
    int           resource_count;

    int           existing  [MAX_RT];
    int           available [MAX_RT];
    int           allocation[MAX_PR][MAX_RT]; /* resources currently held        */
    int           request   [MAX_PR][MAX_RT]; /* pending request (set while blocked) */
    int           max_demand[MAX_PR][MAX_RT]; /* Banker's max claim (avoid only) */
    int           need      [MAX_PR][MAX_RT]; /* max_demand − allocation         */

    pid_t         apid_to_pid[MAX_PR];
    unsigned char started    [MAX_PR]; /* 1 after rsm_process_started */
    unsigned char active     [MAX_PR]; /* 1 until rsm_process_ended   */
    unsigned char claimed    [MAX_PR]; /* 1 after rsm_claim           */
    int           waiting    [MAX_PR]; /* 1 while blocked in rsm_request */

    int           started_count; /* # processes that called rsm_process_started */
    int           claimed_count; /* # processes that called rsm_claim           */

    sem_t         mutex;                /* global mutual-exclusion (value 1)  */
    sem_t         process_sem[MAX_PR];  /* per-process blocking sem (value 0) */
    sem_t         barrier_sem;          /* barrier: open once all register    */
} rsm_shared_state;

/* ====================================================================
 * Per-process globals  (valid only within a single address space)
 * ==================================================================== */

static rsm_shared_state *g_state     = NULL;
static int               g_shm_fd    = -1;
static int               g_local_apid = -1;

/* ====================================================================
 * Internal helpers
 * ==================================================================== */

static int map_existing_state(void)
{
    if (g_state != NULL)
        return 0;

    g_shm_fd = shm_open(RSM_SHM_NAME, O_RDWR, 0666);
    if (g_shm_fd < 0)
        return -1;

    g_state = (rsm_shared_state *)mmap(NULL, sizeof(*g_state),
                                       PROT_READ | PROT_WRITE, MAP_SHARED,
                                       g_shm_fd, 0);
    if (g_state == MAP_FAILED) {
        g_state = NULL;
        close(g_shm_fd);
        g_shm_fd = -1;
        return -1;
    }
    return 0;
}

static void unmap_state(void)
{
    if (g_state != NULL) {
        munmap(g_state, sizeof(*g_state));
        g_state = NULL;
    }
    if (g_shm_fd >= 0) {
        close(g_shm_fd);
        g_shm_fd = -1;
    }
    g_local_apid = -1;
}

static int lock_state(void)
{
    int r;
    if (g_state == NULL) { errno = EINVAL; return -1; }
    do { r = sem_wait(&g_state->mutex); } while (r != 0 && errno == EINTR);
    return r;
}

static int unlock_state(void)
{
    if (g_state == NULL) { errno = EINVAL; return -1; }
    return sem_post(&g_state->mutex);
}

static int validate_vector_bounds(const int vec[], int len)
{
    int i;
    if (!vec) return -1;
    for (i = 0; i < len; ++i)
        if (vec[i] < 0) return -1;
    return 0;
}

/*
 * Returns the apid of the calling process, or -1 if not found.
 * Must be called while the mutex is held.
 */
static int current_apid_locked(void)
{
    int   i;
    pid_t pid;

    if (g_local_apid >= 0 && g_local_apid < g_state->process_count &&
        g_state->started[g_local_apid]                              &&
        g_state->apid_to_pid[g_local_apid] == getpid())
        return g_local_apid;

    pid = getpid();
    for (i = 0; i < g_state->process_count; ++i) {
        if (g_state->started[i] && g_state->apid_to_pid[i] == pid) {
            g_local_apid = i;
            return i;
        }
    }
    return -1;
}

/* ====================================================================
 * Banker's safety algorithm
 *
 * Returns 1 if the system will be in a safe state after granting
 * req[] to process apid, 0 otherwise.
 * Must be called while the mutex is held.
 * ==================================================================== */
static int is_safe(rsm_shared_state *s, int apid, const int req[])
{
    int work  [MAX_RT];
    int finish[MAX_PR];
    int i, j, found;

    /* Tentative available after granting req */
    for (j = 0; j < s->resource_count; ++j) {
        work[j] = s->available[j] - req[j];
        if (work[j] < 0) return 0;           /* not enough resources */
    }

    /* Inactive processes are already "done" */
    for (i = 0; i < s->process_count; ++i)
        finish[i] = !s->active[i];

    /* Safety scan */
    do {
        found = 0;
        for (i = 0; i < s->process_count; ++i) {
            if (finish[i]) continue;
            /*
             * Tentative need for process i:
             * if i == apid, subtract req (simulating the tentative alloc).
             */
            int can = 1;
            for (j = 0; j < s->resource_count; ++j) {
                int need_ij = s->need[i][j] - ((i == apid) ? req[j] : 0);
                if (need_ij > work[j]) { can = 0; break; }
            }
            if (can) {
                /* Process i can finish; "return" its resources */
                for (j = 0; j < s->resource_count; ++j)
                    work[j] += s->allocation[i][j] + ((i == apid) ? req[j] : 0);
                finish[i] = 1;
                found     = 1;
            }
        }
    } while (found);

    for (i = 0; i < s->process_count; ++i)
        if (!finish[i]) return 0;

    return 1;
}

/* ====================================================================
 * Wake and allocate for every blocked process that can now be served.
 * Must be called while the mutex is held.
 * The outer loop handles chained wakeups: allocating for Pi may free
 * resources that make Pj satisfiable.
 * ==================================================================== */
static void try_satisfy_waiting(rsm_shared_state *s)
{
    int i, j;
    int progress = 1;

    while (progress) {
        progress = 0;
        for (i = 0; i < s->process_count; ++i) {
            if (!s->waiting[i] || !s->active[i]) continue;

            /* Raw availability check */
            int avail = 1;
            for (j = 0; j < s->resource_count; ++j) {
                if (s->request[i][j] > s->available[j]) { avail = 0; break; }
            }
            if (!avail) continue;

            /* Avoidance: run Banker's safety check */
            if (s->avoid_enabled && !is_safe(s, i, s->request[i]))
                continue;

            /* Commit allocation */
            for (j = 0; j < s->resource_count; ++j) {
                s->available[j]     -= s->request[i][j];
                s->allocation[i][j] += s->request[i][j];
                if (s->avoid_enabled)
                    s->need[i][j]   -= s->request[i][j];
                s->request[i][j]     = 0;   /* clear pending request */
            }
            s->waiting[i] = 0;
            sem_post(&s->process_sem[i]);   /* wake the blocked process */
            progress = 1;
        }
    }
}

/* ====================================================================
 * API implementation
 * ==================================================================== */

int rsm_init(int p_count, int r_count, int exist[], int avoid)
{
    int               i;
    int               created_fd;
    rsm_shared_state *mapped;

    if (p_count <= 0 || p_count > MAX_PR) return -1;
    if (r_count <= 0 || r_count > MAX_RT) return -1;
    if (validate_vector_bounds(exist, r_count) != 0) return -1;
    if (avoid != 0 && avoid != 1) return -1;

    /* Create shared-memory segment (fails if already exists) */
    created_fd = shm_open(RSM_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (created_fd < 0) return -1;

    if (ftruncate(created_fd, (off_t)sizeof(rsm_shared_state)) != 0)
        goto err_close;

    mapped = (rsm_shared_state *)mmap(NULL, sizeof(*mapped),
                                      PROT_READ | PROT_WRITE, MAP_SHARED,
                                      created_fd, 0);
    if (mapped == MAP_FAILED) goto err_close;

    memset(mapped, 0, sizeof(*mapped));
    mapped->initialized    = 1;
    mapped->avoid_enabled  = avoid;
    mapped->process_count  = p_count;
    mapped->resource_count = r_count;

    for (i = 0; i < r_count; ++i) {
        mapped->existing[i]  = exist[i];
        mapped->available[i] = exist[i];
    }
    for (i = 0; i < p_count; ++i)
        mapped->apid_to_pid[i] = -1;

    /* Global mutex – value 1 (unlocked), pshared=1 for cross-fork use */
    if (sem_init(&mapped->mutex, 1, 1) != 0) goto err_unmap;

    /* Per-process blocking semaphores – value 0 (blocked) */
    for (i = 0; i < p_count; ++i) {
        if (sem_init(&mapped->process_sem[i], 1, 0) != 0) {
            for (--i; i >= 0; --i) sem_destroy(&mapped->process_sem[i]);
            sem_destroy(&mapped->mutex);
            goto err_unmap;
        }
    }

    /* Barrier semaphore – value 0 (closed) */
    if (sem_init(&mapped->barrier_sem, 1, 0) != 0) {
        for (i = 0; i < p_count; ++i) sem_destroy(&mapped->process_sem[i]);
        sem_destroy(&mapped->mutex);
        goto err_unmap;
    }

    unmap_state();
    g_state      = mapped;
    g_shm_fd     = created_fd;
    g_local_apid = -1;
    return 0;

err_unmap:
    munmap(mapped, sizeof(*mapped));
err_close:
    close(created_fd);
    shm_unlink(RSM_SHM_NAME);
    return -1;
}

int rsm_destroy(void)
{
    int i, ret = 0;

    if (map_existing_state() != 0) return -1;

    for (i = 0; i < g_state->process_count; ++i)
        if (sem_destroy(&g_state->process_sem[i]) != 0) ret = -1;

    if (sem_destroy(&g_state->barrier_sem) != 0) ret = -1;
    if (sem_destroy(&g_state->mutex)       != 0) ret = -1;
    if (shm_unlink(RSM_SHM_NAME)           != 0) ret = -1;

    unmap_state();
    return ret;
}

int rsm_process_started(int apid)
{
    int ret = 0;

    if (map_existing_state() != 0) return -1;
    if (apid < 0 || apid >= g_state->process_count) return -1;
    if (lock_state() != 0) return -1;

    if (!g_state->initialized || g_state->started[apid]) {
        ret = -1; goto out;
    }

    g_state->apid_to_pid[apid] = getpid();
    g_state->started[apid]     = 1;
    g_state->active[apid]      = 1;
    g_state->claimed[apid]     = 0;
    memset(g_state->request[apid], 0,
           (size_t)g_state->resource_count * sizeof(int));
    g_local_apid = apid;

    /*
     * Barrier (no-avoidance mode): fire once every process has registered.
     * One sem_post is enough; the relay in rsm_request keeps it open.
     */
    g_state->started_count++;
    if (!g_state->avoid_enabled &&
        g_state->started_count == g_state->process_count)
        sem_post(&g_state->barrier_sem);

out:
    unlock_state();
    return ret;
}

int rsm_process_ended(void)
{
    int i, apid, ret = 0;

    if (map_existing_state() != 0) return -1;
    if (lock_state() != 0) return -1;

    apid = current_apid_locked();
    if (apid < 0 || !g_state->active[apid]) { ret = -1; goto out; }

    /* Release all held resources */
    for (i = 0; i < g_state->resource_count; ++i) {
        g_state->available[i]        += g_state->allocation[apid][i];
        g_state->allocation[apid][i]  = 0;
        g_state->request[apid][i]     = 0;
        g_state->max_demand[apid][i]  = 0;
        g_state->need[apid][i]        = 0;
    }

    g_state->waiting[apid]     = 0;
    g_state->apid_to_pid[apid] = -1;
    g_state->started[apid]     = 0;
    g_state->active[apid]      = 0;
    g_state->claimed[apid]     = 0;

    /* Wake any process that can now be satisfied */
    try_satisfy_waiting(g_state);

out:
    unlock_state();
    if (ret == 0) g_local_apid = -1;
    return ret;
}

int rsm_claim(int claim[])
{
    int i, apid, ret = 0;

    if (map_existing_state() != 0) return -1;
    if (validate_vector_bounds(claim, g_state->resource_count) != 0) return -1;
    if (lock_state() != 0) return -1;

    apid = current_apid_locked();
    if (apid < 0 || !g_state->started[apid]) { ret = -1; goto out; }

    if (!g_state->avoid_enabled) goto out;   /* no-op when avoidance off */
    if (g_state->claimed[apid])  { ret = -1; goto out; } /* already claimed */

    for (i = 0; i < g_state->resource_count; ++i) {
        if (claim[i] > g_state->existing[i]) { ret = -1; goto out; }
    }

    for (i = 0; i < g_state->resource_count; ++i) {
        g_state->max_demand[apid][i] = claim[i];
        g_state->need[apid][i]       = claim[i]; /* alloc is 0 at claim time */
    }
    g_state->claimed[apid] = 1;

    /*
     * Barrier (avoidance mode): fire once every process has submitted its
     * maximum claim.
     */
    g_state->claimed_count++;
    if (g_state->avoid_enabled &&
        g_state->claimed_count == g_state->process_count)
        sem_post(&g_state->barrier_sem);

out:
    unlock_state();
    return ret;
}

int rsm_request(int request[])
{
    int j, apid;

    if (map_existing_state() != 0) return -1;
    if (validate_vector_bounds(request, g_state->resource_count) != 0) return -1;

    /*
     * Barrier: block until all processes have registered.
     *
     * Relay pattern – each process that passes through immediately
     * re-posts, keeping barrier_sem at 1 so every future caller also
     * passes through without blocking.
     */
    {
        int r;
        do { r = sem_wait(&g_state->barrier_sem); } while (r != 0 && errno == EINTR);
        if (r != 0) return -1;
        sem_post(&g_state->barrier_sem);            /* relay */
    }

    /* ---- Validate under lock ---- */
    if (lock_state() != 0) return -1;

    apid = current_apid_locked();
    if (apid < 0 || !g_state->active[apid]) { unlock_state(); return -1; }

    /* request[j] must not exceed existing instances */
    for (j = 0; j < g_state->resource_count; ++j) {
        if (request[j] > g_state->existing[j]) { unlock_state(); return -1; }
    }

    /* Avoidance: request must not exceed remaining need */
    if (g_state->avoid_enabled) {
        for (j = 0; j < g_state->resource_count; ++j) {
            if (request[j] > g_state->need[apid][j]) {
                unlock_state(); return -1;
            }
        }
    }

    /*
     * Store the pending request so that:
     *   1. try_satisfy_waiting can see what this process needs, and
     *   2. rsm_detection can read the request matrix.
     * The row is cleared after successful allocation.
     */
    for (j = 0; j < g_state->resource_count; ++j)
        g_state->request[apid][j] = request[j];

    /* ---- Try to allocate immediately ---- */
    {
        int avail = 1;
        for (j = 0; j < g_state->resource_count; ++j)
            if (request[j] > g_state->available[j]) { avail = 0; break; }

        int safe = avail &&
                   (!g_state->avoid_enabled || is_safe(g_state, apid, request));

        if (avail && safe) {
            for (j = 0; j < g_state->resource_count; ++j) {
                g_state->available[j]        -= request[j];
                g_state->allocation[apid][j] += request[j];
                if (g_state->avoid_enabled)
                    g_state->need[apid][j]   -= request[j];
                g_state->request[apid][j]     = 0;
            }
            unlock_state();
            return 0;
        }
    }

    /*
     * Resources are not available or not safe to grant right now.
     * Mark as waiting BEFORE unlocking so that a concurrent rsm_release
     * cannot miss this process between unlock and sem_wait.
     */
    g_state->waiting[apid] = 1;
    unlock_state();

    {
        int r;
        do { r = sem_wait(&g_state->process_sem[apid]); }
        while (r != 0 && errno == EINTR);
        if (r != 0) return -1;
    }

    /*
     * try_satisfy_waiting (called inside the releaser) has already
     * performed the allocation and cleared request[apid].
     */
    return 0;
}

int rsm_release(int release[])
{
    int j, apid, ret = 0;

    if (map_existing_state() != 0) return -1;
    if (validate_vector_bounds(release, g_state->resource_count) != 0) return -1;
    if (lock_state() != 0) return -1;

    apid = current_apid_locked();
    if (apid < 0 || !g_state->active[apid]) { ret = -1; goto out; }

    /* Cannot release more than currently allocated */
    for (j = 0; j < g_state->resource_count; ++j) {
        if (release[j] > g_state->allocation[apid][j]) { ret = -1; goto out; }
    }

    for (j = 0; j < g_state->resource_count; ++j) {
        g_state->available[j]        += release[j];
        g_state->allocation[apid][j] -= release[j];
        if (g_state->avoid_enabled)
            g_state->need[apid][j]   += release[j];
    }

    /* Wake any process that can now be served */
    try_satisfy_waiting(g_state);

out:
    unlock_state();
    return ret;
}

/*
 * rsm_detection – standard resource-allocation-graph reduction.
 *
 * Initial condition (Silberschatz §7.6):
 *   finish[i] = true  if allocation[i] is all zeros (holds nothing, so
 *                      cannot be in a deadlock cycle).
 *   finish[i] = false otherwise (active process holding ≥1 resource).
 *
 * Returns the number of deadlocked processes, 0 if none, -1 on error.
 */
int rsm_detection(void)
{
    int work  [MAX_RT];
    int finish[MAX_PR];
    int i, j, found, deadlocked;

    if (map_existing_state() != 0) return -1;
    if (lock_state() != 0) return -1;

    for (j = 0; j < g_state->resource_count; ++j)
        work[j] = g_state->available[j];

    for (i = 0; i < g_state->process_count; ++i) {
        if (!g_state->active[i]) { finish[i] = 1; continue; }

        int holds = 0;
        for (j = 0; j < g_state->resource_count; ++j)
            if (g_state->allocation[i][j] > 0) { holds = 1; break; }
        finish[i] = !holds;
    }

    do {
        found = 0;
        for (i = 0; i < g_state->process_count; ++i) {
            if (finish[i]) continue;
            int can = 1;
            for (j = 0; j < g_state->resource_count; ++j)
                if (g_state->request[i][j] > work[j]) { can = 0; break; }
            if (can) {
                for (j = 0; j < g_state->resource_count; ++j)
                    work[j] += g_state->allocation[i][j];
                finish[i] = 1;
                found     = 1;
            }
        }
    } while (found);

    deadlocked = 0;
    for (i = 0; i < g_state->process_count; ++i)
        if (!finish[i]) ++deadlocked;

    unlock_state();
    return deadlocked;
}

void rsm_print_state(char headermsg[])
{
    int i, j;

    if (map_existing_state() != 0) return;
    if (lock_state() != 0) return;

    printf("##########################\n");
    printf("%s\n", headermsg ? headermsg : "");
    printf("###########################\n");

    /* Existing */
    printf("Exist:\n");
    for (j = 0; j < g_state->resource_count; ++j) printf("R%d ", j);
    printf("\n");
    for (j = 0; j < g_state->resource_count; ++j)
        printf("%-3d", g_state->existing[j]);
    printf("\n");

    /* Available */
    printf("Available:\n");
    for (j = 0; j < g_state->resource_count; ++j) printf("R%d ", j);
    printf("\n");
    for (j = 0; j < g_state->resource_count; ++j)
        printf("%-3d", g_state->available[j]);
    printf("\n");

    /* Allocation matrix */
    printf("Allocation:\n");
    for (j = 0; j < g_state->resource_count; ++j) printf("R%d ", j);
    printf("\n");
    for (i = 0; i < g_state->process_count; ++i) {
        printf("P%d:", i);
        for (j = 0; j < g_state->resource_count; ++j)
            printf(" %-2d", g_state->allocation[i][j]);
        printf("\n");
    }

    /* Request matrix */
    printf("Request:\n");
    for (j = 0; j < g_state->resource_count; ++j) printf("R%d ", j);
    printf("\n");
    for (i = 0; i < g_state->process_count; ++i) {
        printf("P%d:", i);
        for (j = 0; j < g_state->resource_count; ++j)
            printf(" %-2d", g_state->request[i][j]);
        printf("\n");
    }

    /* MaxDemand matrix */
    printf("MaxDemand:\n");
    for (j = 0; j < g_state->resource_count; ++j) printf("R%d ", j);
    printf("\n");
    for (i = 0; i < g_state->process_count; ++i) {
        printf("P%d:", i);
        for (j = 0; j < g_state->resource_count; ++j)
            printf(" %-2d", g_state->max_demand[i][j]);
        printf("\n");
    }

    /* Need matrix */
    printf("Need:\n");
    for (j = 0; j < g_state->resource_count; ++j) printf("R%d ", j);
    printf("\n");
    for (i = 0; i < g_state->process_count; ++i) {
        printf("P%d:", i);
        for (j = 0; j < g_state->resource_count; ++j)
            printf(" %-2d", g_state->need[i][j]);
        printf("\n");
    }

    printf("###########################\n");
    fflush(stdout);
    unlock_state();
}