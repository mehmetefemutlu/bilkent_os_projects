#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>

typedef struct {
    int fileIndex;
    int lineNo;
    char *word;
} Occurrence;

typedef struct {
    const char *filePrefix;
    int fileIndex;   // 1..N
    int k;
    Occurrence *arr;
    int size;
    int cap;
    int ok;          // 1 if file opened, 0 if failed
} ThreadResult;

static void message(const char *program) {
    fprintf(stderr, "Usage: %s FilePrefix N K OutFilename\n", program);
}

static void makeInputFilename(char *buffer, size_t buffersize, const char *prefix, int i) {
    int n = snprintf(buffer, buffersize, "%s%d", prefix, i);
    if (n < 0 || (size_t)n >= buffersize) {
        fprintf(stderr, "Filename too long\n");
        exit(1);
    }
}

static void occ_push(Occurrence **arr, int *size, int *cap,
                     const char *word, int fileIndex, int lineNo) {
    if (*size == *cap) {
        int newCap = (*cap == 0) ? 256 : (*cap * 2);
        Occurrence *tmp = realloc(*arr, (size_t)newCap * sizeof(Occurrence));
        if (!tmp) {
            fprintf(stderr, "Out of memory\n");
            exit(1);
        }
        *arr = tmp;
        *cap = newCap;
    }
    (*arr)[*size].word = strdup(word);
    if (!(*arr)[*size].word) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    (*arr)[*size].fileIndex = fileIndex;
    (*arr)[*size].lineNo = lineNo;
    (*size)++;
}

static void free_occurrences(Occurrence *arr, int size) {
    for (int i = 0; i < size; i++) free(arr[i].word);
    free(arr);
}

static int cmp_occurrence(const void *a, const void *b) {
    const Occurrence *oa = (const Occurrence *)a;
    const Occurrence *ob = (const Occurrence *)b;

    int c = strcmp(oa->word, ob->word);
    if (c != 0) return c;
    if (oa->fileIndex != ob->fileIndex) return oa->fileIndex - ob->fileIndex;
    return oa->lineNo - ob->lineNo;
}

static void *worker(void *arg) {
    ThreadResult *tr = (ThreadResult *)arg;

    char filename[512];
    makeInputFilename(filename, sizeof(filename), tr->filePrefix, tr->fileIndex);

    FILE *f = fopen(filename, "r");
    if (!f) {
        tr->ok = 0;
        fprintf(stderr, "Open failed\n", filename);
        return NULL;
    }
    tr->ok = 1;

    char *line = NULL;
    size_t linecap = 0;
    int lineNo = 0;

    while (1) {
        ssize_t nread = getline(&line, &linecap, f);
        if (nread < 0) break;
        lineNo++;

        char *save = NULL;
        for (char *tok = strtok_r(line, " \t\r\n", &save);
             tok != NULL;
             tok = strtok_r(NULL, " \t\r\n", &save)) {

            if ((int)strlen(tok) >= tr->k) {
                occ_push(&tr->arr, &tr->size, &tr->cap, tok, tr->fileIndex, lineNo);
            }
        }
    }

    free(line);
    fclose(f);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        message(argv[0]);
        return 1;
    }

    const char *filePrefix = argv[1];
    int n = atoi(argv[2]);
    int k = atoi(argv[3]);
    const char *outFile = argv[4];

    if (n < 1 || n > 10) {
        fprintf(stderr, "Invalid range for N\n");
        return 1;
    }
    if (k < 1 || k > 100) {
        fprintf(stderr, "Invalid range for K\n");
        return 1;
    }

    pthread_t tids[10];
    ThreadResult results[10];
    memset(results, 0, sizeof(results));

    for (int i = 0; i < n; i++) {
        results[i].filePrefix = filePrefix;
        results[i].fileIndex  = i + 1;  // 1..N
        results[i].k = k;
        results[i].arr = NULL;
        results[i].size = 0;
        results[i].cap = 0;
        results[i].ok = 0;

        if (pthread_create(&tids[i], NULL, worker, &results[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return 1;
        }
    }

    for (int i = 0; i < n; i++) {
        pthread_join(tids[i], NULL);
    }

    // merge
    Occurrence *all = NULL;
    int all_size = 0, all_cap = 0;

    for (int i = 0; i < n; i++) {
        if (!results[i].ok) continue;
        for (int j = 0; j < results[i].size; j++) {
            occ_push(&all, &all_size, &all_cap,
                     results[i].arr[j].word,
                     results[i].arr[j].fileIndex,
                     results[i].arr[j].lineNo);
        }
    }

    // free per-thread arrays
    for (int i = 0; i < n; i++) {
        free_occurrences(results[i].arr, results[i].size);
    }

    if (all_size > 1) {
        qsort(all, (size_t)all_size, sizeof(Occurrence), cmp_occurrence);
    }

    FILE *out = fopen(outFile, "w");
    if (!out) {
        fprintf(stderr, "Failed to open output file\n");
        free_occurrences(all, all_size);
        return 1;
    }

    // group by word + print
    int i = 0;
    while (i < all_size) {
        int j = i + 1;
        while (j < all_size && strcmp(all[i].word, all[j].word) == 0) j++;

        int count = j - i;
        fprintf(out, "%s (count=%d): ", all[i].word, count);
        for (int t = i; t < j; t++) {
            fprintf(out, "%d-%d", all[t].fileIndex, all[t].lineNo);
            if (t + 1 < j) fprintf(out, ", ");
        }
        fprintf(out, "\n");
        i = j;
    }

    fclose(out);
    free_occurrences(all, all_size);
    return 0;
}
