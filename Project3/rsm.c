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

typedef struct {
    int initialized;
    int avoid_enabled;
    int process_count;
    int resource_count;
    int existing[MAX_RT];
    int available[MAX_RT];
    int allocation[MAX_PR][MAX_RT];
    int request[MAX_PR][MAX_RT];
    int max_demand[MAX_PR][MAX_RT];
    int need[MAX_PR][MAX_RT];
    pid_t apid_to_pid[MAX_PR];
    unsigned char started[MAX_PR];
    unsigned char active[MAX_PR];
    unsigned char claimed[MAX_PR];
    sem_t mutex;
} rsm_shared_state;

static rsm_shared_state *g_state;
static int g_shm_fd = -1;
static int g_local_apid = -1;

static int map_existing_state(void)
{
    if (g_state != NULL) {
        return 0;
    }

    g_shm_fd = shm_open(RSM_SHM_NAME, O_RDWR, 0666);
    if (g_shm_fd < 0) {
        return -1;
    }

    g_state = mmap(NULL, sizeof(*g_state), PROT_READ | PROT_WRITE, MAP_SHARED,
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
    if (g_state == NULL) {
        errno = EINVAL;
        return -1;
    }

    return sem_wait(&g_state->mutex);
}

static int unlock_state(void)
{
    if (g_state == NULL) {
        errno = EINVAL;
        return -1;
    }

    return sem_post(&g_state->mutex);
}

static int validate_vector_bounds(const int vec[], int len)
{
    int i;

    if (vec == NULL) {
        return -1;
    }

    for (i = 0; i < len; ++i) {
        if (vec[i] < 0) {
            return -1;
        }
    }

    return 0;
}

static int current_apid_locked(void)
{
    int i;
    pid_t pid;

    if (g_local_apid >= 0 && g_local_apid < g_state->process_count &&
        g_state->started[g_local_apid] &&
        g_state->apid_to_pid[g_local_apid] == getpid()) {
        return g_local_apid;
    }

    pid = getpid();
    for (i = 0; i < g_state->process_count; ++i) {
        if (g_state->started[i] && g_state->apid_to_pid[i] == pid) {
            g_local_apid = i;
            return i;
        }
    }

    return -1;
}

int rsm_init(int p_count, int r_count, int exist[], int avoid)
{
    int i;
    int created_fd;
    rsm_shared_state *mapped;

    if (p_count <= 0 || p_count > MAX_PR || r_count <= 0 || r_count > MAX_RT) {
        return -1;
    }

    if (validate_vector_bounds(exist, r_count) != 0) {
        return -1;
    }

    if (avoid != 0 && avoid != 1) {
        return -1;
    }

    created_fd = shm_open(RSM_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (created_fd < 0) {
        return -1;
    }

    if (ftruncate(created_fd, sizeof(rsm_shared_state)) != 0) {
        close(created_fd);
        shm_unlink(RSM_SHM_NAME);
        return -1;
    }

    mapped = mmap(NULL, sizeof(*mapped), PROT_READ | PROT_WRITE, MAP_SHARED,
                  created_fd, 0);
    if (mapped == MAP_FAILED) {
        close(created_fd);
        shm_unlink(RSM_SHM_NAME);
        return -1;
    }

    memset(mapped, 0, sizeof(*mapped));
    mapped->initialized = 1;
    mapped->avoid_enabled = avoid;
    mapped->process_count = p_count;
    mapped->resource_count = r_count;

    for (i = 0; i < r_count; ++i) {
        mapped->existing[i] = exist[i];
        mapped->available[i] = exist[i];
    }

    for (i = 0; i < p_count; ++i) {
        mapped->apid_to_pid[i] = -1;
    }

    if (sem_init(&mapped->mutex, 1, 1) != 0) {
        munmap(mapped, sizeof(*mapped));
        close(created_fd);
        shm_unlink(RSM_SHM_NAME);
        return -1;
    }

    unmap_state();
    g_state = mapped;
    g_shm_fd = created_fd;
    g_local_apid = -1;

    return 0;
}

int rsm_destroy(void)
{
    int ret = 0;

    if (map_existing_state() != 0) {
        return -1;
    }

    if (sem_destroy(&g_state->mutex) != 0) {
        ret = -1;
    }

    if (shm_unlink(RSM_SHM_NAME) != 0) {
        ret = -1;
    }

    unmap_state();
    return ret;
}

int rsm_process_started(int apid)
{
    pid_t pid;
    int ret = 0;

    if (map_existing_state() != 0) {
        return -1;
    }

    if (apid < 0 || apid >= g_state->process_count) {
        return -1;
    }

    if (lock_state() != 0) {
        return -1;
    }

    pid = getpid();
    if (!g_state->initialized || g_state->started[apid]) {
        ret = -1;
        goto out;
    }

    g_state->apid_to_pid[apid] = pid;
    g_state->started[apid] = 1;
    g_state->active[apid] = 1;
    g_state->claimed[apid] = 0;
    memset(g_state->request[apid], 0, sizeof(g_state->request[apid]));

    g_local_apid = apid;

out:
    if (unlock_state() != 0) {
        ret = -1;
    }

    return ret;
}

int rsm_process_ended(void)
{
    int i;
    int apid;
    int ret = 0;

    if (map_existing_state() != 0) {
        return -1;
    }

    if (lock_state() != 0) {
        return -1;
    }

    apid = current_apid_locked();
    if (apid < 0 || apid >= g_state->process_count || !g_state->started[apid]) {
        ret = -1;
        goto out;
    }

    for (i = 0; i < g_state->resource_count; ++i) {
        g_state->available[i] += g_state->allocation[apid][i];
        g_state->allocation[apid][i] = 0;
        g_state->request[apid][i] = 0;
        g_state->max_demand[apid][i] = 0;
        g_state->need[apid][i] = 0;
    }

    g_state->apid_to_pid[apid] = -1;
    g_state->started[apid] = 0;
    g_state->active[apid] = 0;
    g_state->claimed[apid] = 0;

out:
    if (unlock_state() != 0) {
        ret = -1;
    }

    if (ret == 0) {
        g_local_apid = -1;
    }

    return ret;
}

int rsm_claim(int claim[])
{
    int i;
    int apid;
    int ret = 0;

    if (map_existing_state() != 0) {
        return -1;
    }

    if (validate_vector_bounds(claim, g_state->resource_count) != 0) {
        return -1;
    }

    if (lock_state() != 0) {
        return -1;
    }

    apid = current_apid_locked();
    if (apid < 0 || apid >= g_state->process_count || !g_state->started[apid]) {
        ret = -1;
        goto out;
    }

    if (!g_state->avoid_enabled) {
        goto out;
    }

    if (g_state->claimed[apid]) {
        ret = -1;
        goto out;
    }

    for (i = 0; i < g_state->resource_count; ++i) {
        if (claim[i] > g_state->existing[i] || g_state->allocation[apid][i] != 0) {
            ret = -1;
            goto out;
        }
    }

    for (i = 0; i < g_state->resource_count; ++i) {
        g_state->max_demand[apid][i] = claim[i];
        g_state->need[apid][i] = claim[i] - g_state->allocation[apid][i];
    }
    g_state->claimed[apid] = 1;

out:
    if (unlock_state() != 0) {
        ret = -1;
    }

    return ret;
}

int rsm_request(int request[])
{
    (void)request;
    return -1;
}

int rsm_release(int release[])
{
    (void)release;
    return -1;
}

int rsm_detection(void)
{
    return -1;
}

void rsm_print_state(char headermsg[])
{
    (void)headermsg;
}
