#define _GNU_SOURCE

#include "tus.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>

typedef enum {
    T_READY = 1,
    T_RUNNING,
    T_ENDED
} thread_state_t;

typedef struct TCB {
    int tid;
    thread_state_t state;
    ucontext_t context;
    char *stack;                     
    volatile int resume_flag;        
    volatile int yield_result;     
} TCB;

static TCB *threads[TUS_MAXTHREADS];
static int current_tid = -1;
static int initialized = 0;
static int sched_alg = 0;

static int ready_q[TUS_MAXTHREADS];
static int rq_head = 0;
static int rq_tail = 0;
static int rq_size = 0;

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
    int tid;

    if (rq_empty()) {
        return TUS_ERROR;
    }

    tid = ready_q[rq_head];
    rq_head = (rq_head + 1) % TUS_MAXTHREADS;
    rq_size--;
    return tid;
}

static int rq_push_front(int tid) {
    if (rq_size >= TUS_MAXTHREADS) {
        return TUS_ERROR;
    }

    rq_head = (rq_head - 1 + TUS_MAXTHREADS) % TUS_MAXTHREADS;
    ready_q[rq_head] = tid;
    rq_size++;
    return TUS_SUCCESS;
}

static int rq_remove_tid(int tid) {
    int temp[TUS_MAXTHREADS];
    int temp_size = 0;
    int found = 0;
    int n = rq_size;

    for (int i = 0; i < n; i++) {
        int x = rq_pop();
        if (x == tid && !found) {
            found = 1;
        } else {
            temp[temp_size++] = x;
        }
    }

    for (int i = 0; i < temp_size; i++) {
        rq_push(temp[i]);
    }

    return found ? TUS_SUCCESS : TUS_ERROR;
}


static int valid_sched_alg(int salg) {
    return salg == ALG_FCFS || salg == ALG_RANDOM;
}

static int find_slot_by_tid(int tid) {
    for (int i = 0; i < TUS_MAXTHREADS; i++) {
        if (threads[i] != NULL && threads[i]->tid == tid) {
            return i;
        }
    }
    return -1;
}

static TCB *get_tcb_by_tid(int tid) {
    int slot;

    if (tid <= 0) {
        return NULL;
    }

    slot = find_slot_by_tid(tid);
    return (slot == -1) ? NULL : threads[slot];
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

static int count_existing_threads(void) {
    int count = 0;

    for (int i = 0; i < TUS_MAXTHREADS; i++) {
        if (threads[i] != NULL) {
            count++;
        }
    }

    return count;
}

static int allocate_tid(void) {
    for (int tid = 1; tid <= TUS_MAXTHREADS; tid++) {
        if (get_tcb_by_tid(tid) == NULL) {
            return tid;
        }
    }
    return TUS_ERROR;
}

static void destroy_thread(int tid) {
    int slot = find_slot_by_tid(tid);
    TCB *tcb;

    if (slot == -1) {
        return;
    }

    tcb = threads[slot];
    threads[slot] = NULL;
    free(tcb->stack);
    free(tcb);
}

static int pick_next_tid_by_sched(void) {
    if (rq_empty()) {
        return TUS_ERROR;
    }

    if (sched_alg == ALG_FCFS) {
        return rq_pop();
    }

    if (sched_alg == ALG_RANDOM) {
        int temp[TUS_MAXTHREADS];
        int n = rq_size;
        int idx = rand() % n;
        int picked = TUS_ERROR;

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

static void stub(void *(*tsf)(void *), void *targ) {
    __asm__ volatile("and $-16, %rsp");
    tsf(targ);
    tus_exit();
}

int tus_init(int salg) {
    TCB *main_tcb;
    int slot;

    if (initialized || !valid_sched_alg(salg)) {
        return TUS_ERROR;
    }

    for (int i = 0; i < TUS_MAXTHREADS; i++) {
        threads[i] = NULL;
    }

    rq_init();
    sched_alg = salg;

    main_tcb = (TCB *)malloc(sizeof(TCB));
    if (main_tcb == NULL) {
        return TUS_ERROR;
    }

    memset(main_tcb, 0, sizeof(TCB));
    main_tcb->tid = 1;
    main_tcb->state = T_RUNNING;
    main_tcb->stack = NULL;
    main_tcb->resume_flag = 0;
    main_tcb->yield_result = TUS_ERROR;

    if (getcontext(&main_tcb->context) == -1) {
        free(main_tcb);
        return TUS_ERROR;
    }

    slot = find_free_slot();
    if (slot == -1) {
        free(main_tcb);
        return TUS_ERROR;
    }

    threads[slot] = main_tcb;
    current_tid = main_tcb->tid;
    initialized = 1;
    srand((unsigned)time(NULL));

    return main_tcb->tid;
}

int tus_create_thread(void *(*tsf)(void *), void *targ) {
    TCB *tcb;
    uintptr_t stack_top;
    int slot;
    int tid;

    if (!initialized || tsf == NULL) {
        return TUS_ERROR;
    }

    if (count_existing_threads() >= TUS_MAXTHREADS) {
        return TUS_ERROR;
    }

    slot = find_free_slot();
    if (slot == -1) {
        return TUS_ERROR;
    }

    tid = allocate_tid();
    if (tid == TUS_ERROR) {
        return TUS_ERROR;
    }

    tcb = (TCB *)malloc(sizeof(TCB));
    if (tcb == NULL) {
        return TUS_ERROR;
    }
    memset(tcb, 0, sizeof(TCB));

    tcb->stack = (char *)malloc(TUS_STACKSIZE);
    if (tcb->stack == NULL) {
        free(tcb);
        return TUS_ERROR;
    }

    tcb->tid = tid;
    tcb->state = T_READY;
    tcb->resume_flag = 0;
    tcb->yield_result = TUS_ERROR;

    if (getcontext(&tcb->context) == -1) {
        free(tcb->stack);
        free(tcb);
        return TUS_ERROR;
    }

    stack_top = (uintptr_t)(tcb->stack + TUS_STACKSIZE);

    tcb->context.uc_stack.ss_sp = tcb->stack;
    tcb->context.uc_stack.ss_size = TUS_STACKSIZE;
    tcb->context.uc_link = NULL;
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
    TCB *caller;
    TCB *next_tcb;
    int next_tid_local;

    if (!initialized) {
        return TUS_ERROR;
    }

    caller = current_tcb();
    if (caller == NULL) {
        return TUS_ERROR;
    }

    if (getcontext(&caller->context) == -1) {
        return TUS_ERROR;
    }

    if (caller->resume_flag) {
        caller->resume_flag = 0;
        caller->state = T_RUNNING;
        current_tid = caller->tid;
        return caller->yield_result;
    }
    caller->resume_flag = 1;

    if (tid == TUS_ANY) {
        caller->state = T_READY;
        if (rq_push(caller->tid) == TUS_ERROR) {
            caller->resume_flag = 0;
            caller->state = T_RUNNING;
            return TUS_ERROR;
        }

        next_tid_local = pick_next_tid_by_sched();
        if (next_tid_local == TUS_ERROR) {
            rq_remove_tid(caller->tid);
            caller->resume_flag = 0;
            caller->state = T_RUNNING;
            return TUS_ERROR;
        }
    } else if (tid > 0) {
        if (tid == caller->tid) {
            caller->yield_result = caller->tid;
            caller->state = T_RUNNING;
            caller->resume_flag = 0;
            return caller->tid;
        }

        next_tcb = get_tcb_by_tid(tid);
        if (next_tcb == NULL || next_tcb->state != T_READY) {
            caller->resume_flag = 0;
            return TUS_ERROR;
        }

        if (rq_remove_tid(tid) == TUS_ERROR) {
            caller->resume_flag = 0;
            return TUS_ERROR;
        }

        caller->state = T_READY;
        if (rq_push(caller->tid) == TUS_ERROR) {
            rq_push_front(tid);
            caller->resume_flag = 0;
            caller->state = T_RUNNING;
            return TUS_ERROR;
        }
        next_tid_local = tid;
    } else {
        caller->resume_flag = 0;
        return TUS_ERROR;
    }

    next_tcb = get_tcb_by_tid(next_tid_local);
    if (next_tcb == NULL) {
        if (tid == TUS_ANY) {
            rq_remove_tid(caller->tid);
        }
        caller->resume_flag = 0;
        caller->state = T_RUNNING;
        return TUS_ERROR;
    }

    caller->yield_result = next_tid_local;
    caller->state = T_READY;
    next_tcb->state = T_RUNNING;
    current_tid = next_tid_local;
    setcontext(&next_tcb->context);

    return TUS_ERROR;
}

void tus_exit(void) {
    TCB *caller;
    TCB *next_tcb;
    int next_tid_local;

    if (!initialized) {
        exit(0);
    }

    caller = current_tcb();
    if (caller == NULL) {
        exit(0);
    }

    caller->state = T_ENDED;
    caller->resume_flag = 0;

    next_tid_local = pick_next_tid_by_sched();
    if (next_tid_local == TUS_ERROR) {
        exit(0);
    }

    next_tcb = get_tcb_by_tid(next_tid_local);
    if (next_tcb == NULL) {
        exit(0);
    }

    next_tcb->state = T_RUNNING;
    current_tid = next_tid_local;
    setcontext(&next_tcb->context);

    exit(0);
}

int tus_join(int tid) {
    TCB *caller;
    TCB *target;

    if (!initialized || tid <= 0) {
        return TUS_ERROR;
    }

    caller = current_tcb();
    if (caller == NULL || tid == caller->tid) {
        return TUS_ERROR;
    }

    target = get_tcb_by_tid(tid);
    if (target == NULL) {
        return TUS_ERROR;
    }

    while (target->state != T_ENDED) {
        if (tus_yield(TUS_ANY) == TUS_ERROR) {
            return TUS_ERROR;
        }
        target = get_tcb_by_tid(tid);
        if (target == NULL) {
            return TUS_ERROR;
        }
    }

    destroy_thread(tid);
    return tid;
}

int tus_cancel(int tid) {
    TCB *target;

    if (!initialized || tid <= 0 || tid == current_tid) {
        return TUS_ERROR;
    }

    target = get_tcb_by_tid(tid);
    if (target == NULL || target->state == T_ENDED) {
        return TUS_ERROR;
    }

    if (target->state == T_READY) {
        if (rq_remove_tid(tid) == TUS_ERROR) {
            return TUS_ERROR;
        }
    }

    target->state = T_ENDED;
    target->resume_flag = 0;
    return TUS_SUCCESS;
}

int tus_gettid(void) {
    return current_tid;
}
