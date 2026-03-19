#include <stdio.h>
#include "tus.h"

static void *spinner(void *arg) {
    int rounds = *(int *)arg;

    for (int i = 0; i < rounds; i++) {
        printf("spinner tid=%d round=%d\n", tus_gettid(), i);
        tus_yield(TUS_ANY);
    }

    printf("spinner tid=%d exiting normally\n", tus_gettid());
    tus_exit();
    return NULL;
}

static void *watcher(void *arg) {
    int target_tid = *(int *)arg;

    printf("watcher tid=%d waiting for target %d to terminate\n",
           tus_gettid(), target_tid);

    if (tus_join(target_tid) == TUS_ERROR) {
        printf("watcher tid=%d join failed for %d\n", tus_gettid(), target_tid);
        return NULL;
    }

    printf("watcher tid=%d observed target %d termination\n",
           tus_gettid(), target_tid);
    return NULL;
}

int main(void) {
    int main_tid;
    int spinner_rounds = 20;
    int spinner_tid;
    int watcher_tid;

    main_tid = tus_init(ALG_RANDOM);
    if (main_tid == TUS_ERROR) {
        fprintf(stderr, "tus_init failed\n");
        return 1;
    }

    spinner_tid = tus_create_thread(spinner, &spinner_rounds);
    if (spinner_tid == TUS_ERROR) {
        fprintf(stderr, "spinner creation failed\n");
        return 1;
    }

    watcher_tid = tus_create_thread(watcher, &spinner_tid);
    if (watcher_tid == TUS_ERROR) {
        fprintf(stderr, "watcher creation failed\n");
        return 1;
    }

    printf("main tid=%d created spinner=%d watcher=%d\n",
           main_tid, spinner_tid, watcher_tid);

    for (int i = 0; i < 3; i++) {
        printf("main tid=%d yielding before cancel step %d\n", main_tid, i);
        tus_yield(TUS_ANY);
    }

    if (tus_cancel(spinner_tid) == TUS_ERROR) {
        fprintf(stderr, "cancel failed for %d\n", spinner_tid);
        return 1;
    }

    printf("main tid=%d cancelled spinner %d\n", main_tid, spinner_tid);

    if (tus_join(watcher_tid) == TUS_ERROR) {
        fprintf(stderr, "join failed for watcher %d\n", watcher_tid);
        return 1;
    }

    printf("cancel test completed\n");
    return 0;
}
