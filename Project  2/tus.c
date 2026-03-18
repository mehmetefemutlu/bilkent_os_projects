#include "tus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

/* =========================
   Internal thread metadata
   ========================= */

typedef enum {
    T_FREE = 0,
    T_READY,
    T_RUNNING,
    T_WAITING,
    T_ENDED
} thread_state_t;

typedef struct TCB {
    int tid;
    thread_state_t state;
    ucontext_t context;
    char *stack;                 /* NULL for main thread */
    void *(*start_func)(void *);
    void *arg;
    int waiting_for;             /* for join later, -1 if none */
} TCB;

/* =========================
   Global library state
   ========================= */

static TCB *threads[TUS_MAXTHREADS];
static int current_tid = -1;
static int next_tid = 1;
static int initialized = 0;
static int sched_alg = 0;

/* Ready queue as circular array */
static int ready_q[TUS_MAXTHREADS];
static int rq_head = 0;
static int rq_tail = 0;
static int rq_size = 0;

/* =========================
   Ready queue helpers
   ========================= */

static void rq_init(void) {
    rq_head = 0;
    rq_tail = 0;
    rq_size = 0;
}

static int rq_empty(void) {
    return rq_size == 0;
}

static int rq_push(int tid) {
    if (rq_size >= TUS_MAXTHREADS) {
        return TUS_ERROR;
    }
    ready_q[rq_tail] = tid;
    rq_tail = (rq_tail + 1) % TUS_MAXTHREADS;
    rq_size++;
    return TUS_SUCCESS;
}

static int rq_pop(void) {
    if (rq_empty()) {
        return TUS_ERROR;
    }
    int tid = ready_q[rq_head];
    rq_head = (rq_head + 1) % TUS_MAXTHREADS;
    rq_size--;
    return tid;
}

/* For now, simple linear remove by rebuilding queue */
static int rq_remove_tid(int tid) {
    if (rq_empty()) {
        return TUS_ERROR;
    }

    int temp[TUS_MAXTHREADS];
    int temp_size = 0;
    int found = 0;

    while (!rq_empty()) {
        int x = rq_pop();
        if (x == tid && !found) {
            found = 1;
            continue;
        }
        temp[temp_size++] = x;
    }

    for (int i = 0; i < temp_size; i++) {
        rq_push(temp[i]);
    }

    return found ? TUS_SUCCESS : TUS_ERROR;
}

/* =========================
   Internal helpers
   ========================= */

static int valid_sched_alg(int salg) {
    return (salg == ALG_FCFS || salg == ALG_RANDOM);
}

static TCB *get_tcb_by_tid(int tid) {
    if (tid <= 0 || tid >= next_tid) {
        return NULL;
    }
    for (int i = 0; i < TUS_MAXTHREADS; i++) {
        if (threads[i] != NULL && threads[i]->tid == tid) {
            return threads[i];
        }
    }
    return NULL;
}

static int find_free_slot(void) {
    for (int i = 0; i < TUS_MAXTHREADS; i++) {
        if (threads[i] == NULL) {
            return i;
        }
    }
    return -1;
}

static TCB *current_tcb(void) {
    return get_tcb_by_tid(current_tid);
}

static int pick_next_tid_by_sched(void) {
    if (rq_empty()) {
        return TUS_ERROR;
    }

    if (sched_alg == ALG_FCFS) {
        return rq_pop();
    }

    if (sched_alg == ALG_RANDOM) {
        int idx = rand() % rq_size;

        int temp[TUS_MAXTHREADS];
        int picked = -1;
        int n = rq_size;

        for (int i = 0; i < n; i++) {
            temp[i] = rq_pop();
        }

        for (int i = 0; i < n; i++) {
            if (i == idx) {
                picked = temp[i];
            } else {
                rq_push(temp[i]);
            }
        }

        return picked;
    }

    return TUS_ERROR;
}

/* =========================
   API: Part 1 -> tus_init()
   ========================= */

int tus_init(int salg) {
    if (initialized) {
        return TUS_ERROR;
    }

    if (!valid_sched_alg(salg)) {
        return TUS_ERROR;
    }

    sched_alg = salg;

    for (int i = 0; i < TUS_MAXTHREADS; i++) {
        threads[i] = NULL;
    }

    rq_init();

    TCB *main_tcb = (TCB *)malloc(sizeof(TCB));
    if (main_tcb == NULL) {
        return TUS_ERROR;
    }

    memset(main_tcb, 0, sizeof(TCB));

    main_tcb->tid = next_tid++;          /* main thread can be 1 */
    main_tcb->state = T_RUNNING;
    main_tcb->stack = NULL;              /* assignment says main already has a stack */
    main_tcb->start_func = NULL;
    main_tcb->arg = NULL;
    main_tcb->waiting_for = -1;

    if (getcontext(&main_tcb->context) == -1) {
        free(main_tcb);
        return TUS_ERROR;
    }

    int slot = find_free_slot();
    if (slot == -1) {
        free(main_tcb);
        return TUS_ERROR;
    }

    threads[slot] = main_tcb;
    current_tid = main_tcb->tid;
    initialized = 1;

    return main_tcb->tid;
}

/* =========================
   Stubs for next parts
   ========================= */

static void stub(void *(*tsf)(void *), void *targ) {
    __asm__ volatile("and $-16, %rsp");
    tsf(targ);
    tus_exit();
}

int tus_create_thread(void *(*tsf)(void *), void *targ) {
    if (!initialized || tsf == NULL) {
        return TUS_ERROR;
    }

    int slot = find_free_slot();
    if (slot == -1) {
        return TUS_ERROR;
    }

    if (next_tid > TUS_MAXTHREADS) {
        return TUS_ERROR;
    }

    TCB *tcb = (TCB *)malloc(sizeof(TCB));
    if (tcb == NULL) {
        return TUS_ERROR;
    }
    memset(tcb, 0, sizeof(TCB));

    tcb->stack = (char *)malloc(TUS_STACKSIZE);
    if (tcb->stack == NULL) {
        free(tcb);
        return TUS_ERROR;
    }

    tcb->tid = next_tid++;
    tcb->state = T_READY;
    tcb->start_func = tsf;
    tcb->arg = targ;
    tcb->waiting_for = -1;

    /*
     * Initial context is copied from current running thread context.
     * Assignment explicitly says getcontext() should be used here.
     */
    if (getcontext(&tcb->context) == -1) {
        free(tcb->stack);
        free(tcb);
        return TUS_ERROR;
    }

    /*
     * New thread will start from stub().
     * Stack grows downward, so top = base + size.
     */
    uintptr_t stack_top = (uintptr_t)(tcb->stack + TUS_STACKSIZE);

    /*
     * Fill architecture-specific registers for x86-64.
     */
    tcb->context.uc_mcontext.gregs[REG_RIP] = (greg_t)stub;
    tcb->context.uc_mcontext.gregs[REG_RSP] = (greg_t)stack_top;
    tcb->context.uc_mcontext.gregs[REG_RDI] = (greg_t)tsf;
    tcb->context.uc_mcontext.gregs[REG_RSI] = (greg_t)targ;

    threads[slot] = tcb;

    if (rq_push(tcb->tid) == TUS_ERROR) {
        threads[slot] = NULL;
        free(tcb->stack);
        free(tcb);
        return TUS_ERROR;
    }

    return tcb->tid;
}

int tus_yield(int tid) {
    if (!initialized) {
        return TUS_ERROR;
    }

    TCB *caller = current_tcb();
    if (caller == NULL) {
        return TUS_ERROR;
    }

    /*
     * Save caller context first.
     * getcontext() returns twice:
     *  - first: right after saving caller context
     *  - second: when caller is scheduled again later with setcontext()
     */
    volatile int resumed = 0;
    if (getcontext(&caller->context) == -1) {
        return TUS_ERROR;
    }

    if (resumed == 1) {
        return current_tid;
    }
    resumed = 1;

    int next_tid = -1;
    TCB *next_tcb = NULL;

    /*
     * If yielding to a specific positive tid:
     * - it must exist
     * - it must be READY
     * - if not, return immediately with error
     */
    if (tid > 0) {
        next_tcb = get_tcb_by_tid(tid);
        if (next_tcb == NULL || next_tcb->state != T_READY) {
            return TUS_ERROR;
        }

        next_tid = tid;

        if (rq_remove_tid(next_tid) == TUS_ERROR) {
            return TUS_ERROR;
        }
    }
    else if (tid == TUS_ANY) {
        /*
         * Caller becomes READY and is inserted into ready queue
         * before scheduler picks next.
         */
        caller->state = T_READY;
        if (rq_push(caller->tid) == TUS_ERROR) {
            return TUS_ERROR;
        }

        next_tid = pick_next_tid_by_sched();
        if (next_tid == TUS_ERROR) {
            return TUS_ERROR;
        }

        next_tcb = get_tcb_by_tid(next_tid);
        if (next_tcb == NULL) {
            return TUS_ERROR;
        }
    }
    else {
        return TUS_ERROR;
    }

    /*
     * Specific-tid case:
     * caller should also become READY and go to queue,
     * unless caller is somehow yielding to itself.
     */
    if (tid > 0) {
        caller->state = T_READY;
        if (rq_push(caller->tid) == TUS_ERROR) {
            return TUS_ERROR;
        }
    }

    /*
     * Switch to selected thread
     */
    caller->state = T_READY;
    next_tcb->state = T_RUNNING;
    current_tid = next_tcb->tid;

    setcontext(&next_tcb->context);

    /*
     * Should never reach here
     */
    return TUS_ERROR;
}

void tus_exit(void) {
    exit(0);
}

int tus_join(int tid) {
    (void)tid;
    return TUS_ERROR;
}

int tus_cancel(int tid) {
    (void)tid;
    return TUS_ERROR;
}

int tus_gettid(void) {
    return current_tid;
}

