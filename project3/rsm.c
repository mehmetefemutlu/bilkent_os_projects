#include "rsm.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
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
    unsigned char requested_once[MAX_PR];
    int waiting[MAX_PR];

    sem_t mutex;
    sem_t process_sem[MAX_PR];
} rsm_shared_state;

static rsm_shared_state *g_state = NULL;
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

    if (g_state == NULL) {
        errno = EINVAL;
        return -1;
    }

    do {
        r = sem_wait(&g_state->mutex);
    } while (r != 0 && errno == EINTR);

    return r;
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

    if (g_local_apid >= 0 &&
        g_local_apid < g_state->process_count &&
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

static int is_safe(rsm_shared_state *s, int apid, const int req[])
{
    int work[MAX_RT];
    int finish[MAX_PR];
    int i;
    int j;
    int found;

    for (j = 0; j < s->resource_count; ++j) {
        work[j] = s->available[j] - req[j];
        if (work[j] < 0) {
            return 0;
        }
    }

    for (i = 0; i < s->process_count; ++i) {
        finish[i] = !s->active[i];
    }

    do {
        found = 0;
        for (i = 0; i < s->process_count; ++i) {
            int can = 1;

            if (finish[i]) {
                continue;
            }

            for (j = 0; j < s->resource_count; ++j) {
                int need_ij = s->need[i][j] - ((i == apid) ? req[j] : 0);
                if (need_ij > work[j]) {
                    can = 0;
                    break;
                }
            }

            if (can) {
                for (j = 0; j < s->resource_count; ++j) {
                    work[j] += s->allocation[i][j] + ((i == apid) ? req[j] : 0);
                }
                finish[i] = 1;
                found = 1;
            }
        }
    } while (found);

    for (i = 0; i < s->process_count; ++i) {
        if (!finish[i]) {
            return 0;
        }
    }

    return 1;
}

static void try_satisfy_waiting(rsm_shared_state *s)
{
    int i;
    int j;
    int progress = 1;

    while (progress) {
        progress = 0;
        for (i = 0; i < s->process_count; ++i) {
            int avail = 1;

            if (!s->waiting[i] || !s->active[i]) {
                continue;
            }

            for (j = 0; j < s->resource_count; ++j) {
                if (s->request[i][j] > s->available[j]) {
                    avail = 0;
                    break;
                }
            }
            if (!avail) {
                continue;
            }

            if (s->avoid_enabled && !is_safe(s, i, s->request[i])) {
                continue;
            }

            for (j = 0; j < s->resource_count; ++j) {
                s->available[j] -= s->request[i][j];
                s->allocation[i][j] += s->request[i][j];
                if (s->avoid_enabled) {
                    s->need[i][j] -= s->request[i][j];
                }
                s->request[i][j] = 0;
            }

            s->waiting[i] = 0;
            sem_post(&s->process_sem[i]);
            progress = 1;
        }
    }
}

int rsm_init(int p_count, int r_count, int exist[], int avoid)
{
    int i;
    int created_fd;
    rsm_shared_state *mapped;

    if (p_count <= 0 || p_count > MAX_PR) return -1;
    if (r_count <= 0 || r_count > MAX_RT) return -1;
    if (validate_vector_bounds(exist, r_count) != 0) return -1;
    if (avoid != 0 && avoid != 1) return -1;

    created_fd = shm_open(RSM_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (created_fd < 0) return -1;

    if (ftruncate(created_fd, (off_t)sizeof(rsm_shared_state)) != 0) {
        close(created_fd);
        shm_unlink(RSM_SHM_NAME);
        return -1;
    }

    mapped = (rsm_shared_state *)mmap(NULL, sizeof(*mapped),
                                      PROT_READ | PROT_WRITE, MAP_SHARED,
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

    for (i = 0; i < p_count; ++i) {
        if (sem_init(&mapped->process_sem[i], 1, 0) != 0) {
            for (--i; i >= 0; --i) {
                sem_destroy(&mapped->process_sem[i]);
            }
            sem_destroy(&mapped->mutex);
            munmap(mapped, sizeof(*mapped));
            close(created_fd);
            shm_unlink(RSM_SHM_NAME);
            return -1;
        }
    }

    unmap_state();
    g_state = mapped;
    g_shm_fd = created_fd;
    g_local_apid = -1;
    return 0;
}

int rsm_destroy(void)
{
    int i;
    int ret = 0;

    if (map_existing_state() != 0) return -1;

    for (i = 0; i < g_state->process_count; ++i) {
        if (sem_destroy(&g_state->process_sem[i]) != 0) {
            ret = -1;
        }
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
    int ret = 0;

    if (map_existing_state() != 0) return -1;
    if (apid < 0 || apid >= g_state->process_count) return -1;
    if (lock_state() != 0) return -1;

    if (!g_state->initialized || g_state->started[apid]) {
        ret = -1;
        goto out;
    }

    g_state->apid_to_pid[apid] = getpid();
    g_state->started[apid] = 1;
    g_state->active[apid] = 1;
    g_state->claimed[apid] = 0;
    g_state->requested_once[apid] = 0;
    g_state->waiting[apid] = 0;
    memset(g_state->request[apid], 0,
           (size_t)g_state->resource_count * sizeof(int));
    g_local_apid = apid;

out:
    unlock_state();
    return ret;
}

int rsm_process_ended(void)
{
    int i;
    int apid;
    int ret = 0;

    if (map_existing_state() != 0) return -1;
    if (lock_state() != 0) return -1;

    apid = current_apid_locked();
    if (apid < 0 || !g_state->active[apid]) {
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

    g_state->waiting[apid] = 0;
    g_state->apid_to_pid[apid] = -1;
    g_state->started[apid] = 0;
    g_state->active[apid] = 0;
    g_state->claimed[apid] = 0;
    g_state->requested_once[apid] = 0;

    try_satisfy_waiting(g_state);

out:
    unlock_state();
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

    if (map_existing_state() != 0) return -1;
    if (validate_vector_bounds(claim, g_state->resource_count) != 0) return -1;
    if (lock_state() != 0) return -1;

    apid = current_apid_locked();
    if (apid < 0 || !g_state->started[apid]) {
        ret = -1;
        goto out;
    }

    if (!g_state->avoid_enabled) {
        goto out;
    }
    if (g_state->claimed[apid] || g_state->requested_once[apid]) {
        ret = -1;
        goto out;
    }

    for (i = 0; i < g_state->resource_count; ++i) {
        if (claim[i] > g_state->existing[i]) {
            ret = -1;
            goto out;
        }
    }

    for (i = 0; i < g_state->resource_count; ++i) {
        g_state->max_demand[apid][i] = claim[i];
        g_state->need[apid][i] = claim[i];
    }
    g_state->claimed[apid] = 1;

out:
    unlock_state();
    return ret;
}

int rsm_request(int request[])
{
    int j;
    int apid;

    if (map_existing_state() != 0) return -1;
    if (validate_vector_bounds(request, g_state->resource_count) != 0) return -1;
    if (lock_state() != 0) return -1;

    apid = current_apid_locked();
    if (apid < 0 || !g_state->active[apid]) {
        unlock_state();
        return -1;
    }

    for (j = 0; j < g_state->resource_count; ++j) {
        if (request[j] > g_state->existing[j]) {
            unlock_state();
            return -1;
        }
    }

    if (g_state->avoid_enabled) {
        if (!g_state->claimed[apid]) {
            unlock_state();
            return -1;
        }
        for (j = 0; j < g_state->resource_count; ++j) {
            if (request[j] > g_state->need[apid][j]) {
                unlock_state();
                return -1;
            }
        }
    }

    g_state->requested_once[apid] = 1;

    for (j = 0; j < g_state->resource_count; ++j) {
        g_state->request[apid][j] = request[j];
    }

    {
        int avail = 1;
        int safe;

        for (j = 0; j < g_state->resource_count; ++j) {
            if (request[j] > g_state->available[j]) {
                avail = 0;
                break;
            }
        }

        safe = avail && (!g_state->avoid_enabled || is_safe(g_state, apid, request));
        if (avail && safe) {
            for (j = 0; j < g_state->resource_count; ++j) {
                g_state->available[j] -= request[j];
                g_state->allocation[apid][j] += request[j];
                if (g_state->avoid_enabled) {
                    g_state->need[apid][j] -= request[j];
                }
                g_state->request[apid][j] = 0;
            }
            unlock_state();
            return 0;
        }
    }

    g_state->waiting[apid] = 1;
    unlock_state();

    {
        int r;
        do {
            r = sem_wait(&g_state->process_sem[apid]);
        } while (r != 0 && errno == EINTR);
        if (r != 0) {
            return -1;
        }
    }

    return 0;
}

int rsm_release(int release[])
{
    int j;
    int apid;
    int ret = 0;

    if (map_existing_state() != 0) return -1;
    if (validate_vector_bounds(release, g_state->resource_count) != 0) return -1;
    if (lock_state() != 0) return -1;

    apid = current_apid_locked();
    if (apid < 0 || !g_state->active[apid]) {
        ret = -1;
        goto out;
    }

    for (j = 0; j < g_state->resource_count; ++j) {
        if (release[j] > g_state->allocation[apid][j]) {
            ret = -1;
            goto out;
        }
    }

    for (j = 0; j < g_state->resource_count; ++j) {
        g_state->available[j] += release[j];
        g_state->allocation[apid][j] -= release[j];
        if (g_state->avoid_enabled) {
            g_state->need[apid][j] += release[j];
        }
    }

    try_satisfy_waiting(g_state);

out:
    unlock_state();
    return ret;
}

int rsm_detection(void)
{
    int work[MAX_RT];
    int finish[MAX_PR];
    int i;
    int j;
    int found;
    int deadlocked;

    if (map_existing_state() != 0) return -1;
    if (lock_state() != 0) return -1;

    for (j = 0; j < g_state->resource_count; ++j) {
        work[j] = g_state->available[j];
    }

    for (i = 0; i < g_state->process_count; ++i) {
        int holds = 0;

        if (!g_state->active[i]) {
            finish[i] = 1;
            continue;
        }

        for (j = 0; j < g_state->resource_count; ++j) {
            if (g_state->allocation[i][j] > 0) {
                holds = 1;
                break;
            }
        }
        finish[i] = !holds;
    }

    do {
        found = 0;
        for (i = 0; i < g_state->process_count; ++i) {
            int can = 1;

            if (finish[i]) {
                continue;
            }

            for (j = 0; j < g_state->resource_count; ++j) {
                if (g_state->request[i][j] > work[j]) {
                    can = 0;
                    break;
                }
            }

            if (can) {
                for (j = 0; j < g_state->resource_count; ++j) {
                    work[j] += g_state->allocation[i][j];
                }
                finish[i] = 1;
                found = 1;
            }
        }
    } while (found);

    deadlocked = 0;
    for (i = 0; i < g_state->process_count; ++i) {
        if (!finish[i]) {
            ++deadlocked;
        }
    }

    unlock_state();
    return deadlocked;
}

void rsm_print_state(char headermsg[])
{
    int i;
    int j;

    if (map_existing_state() != 0) return;
    if (lock_state() != 0) return;

    printf("########################## %s ###########################\n",
           headermsg ? headermsg : "");

    printf("Exist:\n");
    printf("     ");
    for (j = 0; j < g_state->resource_count; ++j) printf(" R%d", j);
    printf("\n");
    printf("     ");
    for (j = 0; j < g_state->resource_count; ++j) printf("%3d", g_state->existing[j]);
    printf("\n");

    printf("Available:\n");
    printf("     ");
    for (j = 0; j < g_state->resource_count; ++j) printf(" R%d", j);
    printf("\n");
    printf("     ");
    for (j = 0; j < g_state->resource_count; ++j) printf("%3d", g_state->available[j]);
    printf("\n");

    printf("Allocation:\n");
    printf("     ");
    for (j = 0; j < g_state->resource_count; ++j) printf(" R%d", j);
    printf("\n");
    for (i = 0; i < g_state->process_count; ++i) {
        printf("P%d: ", i);
        for (j = 0; j < g_state->resource_count; ++j) printf("%3d", g_state->allocation[i][j]);
        printf("\n");
    }

    printf("Request:\n");
    printf("     ");
    for (j = 0; j < g_state->resource_count; ++j) printf(" R%d", j);
    printf("\n");
    for (i = 0; i < g_state->process_count; ++i) {
        printf("P%d: ", i);
        for (j = 0; j < g_state->resource_count; ++j) printf("%3d", g_state->request[i][j]);
        printf("\n");
    }

    printf("MaxDemand:\n");
    printf("     ");
    for (j = 0; j < g_state->resource_count; ++j) printf(" R%d", j);
    printf("\n");
    for (i = 0; i < g_state->process_count; ++i) {
        printf("P%d: ", i);
        for (j = 0; j < g_state->resource_count; ++j) printf("%3d", g_state->max_demand[i][j]);
        printf("\n");
    }

    printf("Need:\n");
    printf("     ");
    for (j = 0; j < g_state->resource_count; ++j) printf(" R%d", j);
    printf("\n");
    for (i = 0; i < g_state->process_count; ++i) {
        printf("P%d: ", i);
        for (j = 0; j < g_state->resource_count; ++j) printf("%3d", g_state->need[i][j]);
        printf("\n");
    }

    printf("###########################\n");
    fflush(stdout);
    unlock_state();
}
