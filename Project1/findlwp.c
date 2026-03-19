#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <sys/types.h>
#include <ctype.h>
#define R 0
#define W 1


typedef struct {
	int fileIndex;
	int lineNo;
	char* word;
} Occurrence;

static void message(const char* program){
  fprintf(stderr, "Usage: %s FilePrefix N K DataLen OutFilename\n", program);
};

static void makeInputFilename(char* buffer, size_t buffersize, const char* prefix, int i) {
	int n = snprintf(buffer, buffersize, "%s%d", prefix, i);
	if (n < 0 || (size_t)n >= buffersize) {
		fprintf(stderr, "Filename too long\n");
		exit(1);
  	}
}


static int writeToParent(int fd, const char* buffer, size_t length, int dataLength){
	size_t off = 0;
	while (off < length) {
    		size_t chunk = length - off;
    		if (chunk > (size_t)dataLength)
			chunk = (size_t)dataLength;

    		ssize_t w_data = write(fd, buffer + off, chunk);

		if(w_data < 0){
			if (errno == EINTR) continue;
      			return -1;
		}

    		off += (size_t)w_data;
  	}
  	return 0;
}

static void pipe_push(Occurrence** arr, int* size, int* capacity, const char* word, int fileIndex, int lineNo){
	if (*size == *capacity) {
    		int newCapacity = (*capacity == 0) ? 256 : (*capacity * 2);
  		Occurrence *temp = (Occurrence*)realloc(*arr, (size_t)newCapacity * sizeof(Occurrence));
 		if (!temp) {
      			fprintf(stderr, "Out of memory\n");
      			exit(1);
    		}
    		*arr = temp;
    		*capacity = newCapacity;
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

static void free_occurrences(Occurrence* arr, int size){
	for (int i = 0; i < size; i++) {
		free(arr[i].word);
	}
	free(arr);
}

static int cmp_occurrence(const void* a, const void* b){
	const Occurrence* oa = (const Occurrence*)a;
	const Occurrence* ob = (const Occurrence*)b;
	int c = strcmp(oa->word, ob->word);
	if (c != 0) return c;
	if (oa->fileIndex != ob->fileIndex) return oa->fileIndex - ob->fileIndex;
	return oa->lineNo - ob->lineNo;
}

static int parse_record_line(const char* line, char** out_word, int* out_file, int* out_line){
	const char* p = line;
	while (*p && isspace((unsigned char)*p)) p++;
	if (*p == '\0') return -1;

	const char* w_start = p;
	while (*p && !isspace((unsigned char)*p)) p++;
	if (p == w_start) return -1;
	size_t w_len = (size_t)(p - w_start);
	char* word = (char*)malloc(w_len + 1);
	if (!word) return -1;
	memcpy(word, w_start, w_len);
	word[w_len] = '\0';

	while (*p && isspace((unsigned char)*p)) p++;
	if (*p == '\0') {
		free(word);
		return -1;
	}
	char* endptr = NULL;
	long file_idx = strtol(p, &endptr, 10);
	if (endptr == p) {
		free(word);
		return -1;
	}
	p = endptr;
	while (*p && isspace((unsigned char)*p)) p++;
	if (*p == '\0') {
		free(word);
		return -1;
	}
	long line_no = strtol(p, &endptr, 10);
	if (endptr == p) {
		free(word);
		return -1;
	}

	*out_word = word;
	*out_file = (int)file_idx;
	*out_line = (int)line_no;
	return 0;
}


static void child_process(const char* filePrefix, int n, int k, int fileIndex, int dataLength, int writefd){
	(void)n;
	char filename[512];
	makeInputFilename(filename, sizeof(filename), filePrefix, fileIndex);

	FILE* f = fopen(filename, "r");

	if(!f){
		close(writefd);
		_exit(2);
	}

	Occurrence* o = NULL;
  	int size = 0;
	int capacity = 0;

  	char* line = NULL;
  	size_t linecapacity = 0;
  	int lineNo = 0;

  	while (true) {
    		ssize_t nread = getline(&line, &linecapacity, f);
    		if (nread < 0)
			break;
    		lineNo++;

    		char* save = NULL;
    		for (char* token = strtok_r(line, " \t\r\n", &save); token != NULL; token = strtok_r(NULL, " \t\r\n", &save)) {
      			if ((int)strlen(token) >= k) {
       				pipe_push(&o, &size, &capacity, token, fileIndex, lineNo);
      			}
    		}
  	}

  	free(line);
  	fclose(f);

	for (int i = 0; i < size; i++) {
		int m = snprintf(NULL, 0, "%s %d %d\n", o[i].word, o[i].fileIndex, o[i].lineNo);
		if (m <= 0) continue;
		char* rec = (char*)malloc((size_t)m + 1);
		if (!rec) {
			fprintf(stderr, "Out of memory\n");
			break;
		}
		snprintf(rec, (size_t)m + 1, "%s %d %d\n", o[i].word, o[i].fileIndex, o[i].lineNo);
		writeToParent(writefd, rec, (size_t)m, dataLength);
		free(rec);
  	}
	close(writefd);
	free_occurrences(o, size);
}

int main(int argc, char** argv){

	if(argc != 6){
      message(argv[0]);
      return 1;
      }

      const char* filePrefix = argv[1];
      int n = atoi(argv[2]);
      int k = atoi(argv[3]);
      int dataLength = atoi(argv[4]);
      const char* outputFileName = argv[5];

      if(n < 1 || n > 10){
          fprintf(stderr, "Invalid range for N\n");
          return 1;
      }

      if(k < 1 || k > 100){
          fprintf(stderr, "Invalid range for K\n");
          return 1;
      }

      if (dataLength < 1 || dataLength > 1000) {
          fprintf(stderr, "Invalid range for data length\n");
          return 1;
      }

      int fd[n][2];

      for(int i = 0; i < n; i++){ //n pipe creation for n child
        if(pipe(fd[i]) < 0) {
          fprintf(stderr, "Pipe failed\n");
          return 1;
        }
  	  }


  	pid_t pids [n];

  	for(int i = 0; i < n; i++){ //n child creation for n pipes
      pid_t pid = fork();
      if(pid < 0) {
          fprintf(stderr, "Fork failed\n");
          return 1;
      }
      if(pid == 0){ //child process
        for(int j = 0; j < n; j++){
        close(fd[j][0]);
        if(j != i){
            close(fd[j][1]);
        }
        }
        child_process(filePrefix, n, k, i+1, dataLength, fd[i][1]);
        _exit(0);
      }

      //parent process
      pids[i] = pid;
      close(fd[i][1]);
  	}

	Occurrence* all = NULL;
	int all_size = 0;
	int all_capacity = 0;

	char** buffers = (char**)calloc((size_t)n, sizeof(char*));
	size_t* buf_len = (size_t*)calloc((size_t)n, sizeof(size_t));
	size_t* buf_cap = (size_t*)calloc((size_t)n, sizeof(size_t));
	bool* closed = (bool*)calloc((size_t)n, sizeof(bool));
	if (!buffers || !buf_len || !buf_cap || !closed) {
		fprintf(stderr, "Out of memory\n");
		return 1;
	}

	int open_pipes = n;
	while (open_pipes > 0) {
		fd_set readset;
		FD_ZERO(&readset);
		int maxfd = -1;
		for (int i = 0; i < n; i++) {
			if (closed[i]) continue;
			FD_SET(fd[i][0], &readset);
			if (fd[i][0] > maxfd) maxfd = fd[i][0];
		}

		int sel;
		do {
			sel = select(maxfd + 1, &readset, NULL, NULL, NULL);
		} while (sel < 0 && errno == EINTR);
		if (sel < 0) {
			fprintf(stderr, "select() failed\n");
			break;
		}

		for (int i = 0; i < n; i++) {
			if (closed[i]) continue;
			if (!FD_ISSET(fd[i][0], &readset)) continue;

			char tmp[1024];
			size_t chunk = (size_t)dataLength;
			if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
			ssize_t r = read(fd[i][0], tmp, chunk);
			if (r == 0) {
				close(fd[i][0]);
				closed[i] = true;
				open_pipes--;
				continue;
			}
			if (r < 0) {
				if (errno == EINTR) continue;
				fprintf(stderr, "read() failed\n");
				close(fd[i][0]);
				closed[i] = true;
				open_pipes--;
				continue;
			}

			if (buf_len[i] + (size_t)r + 1 > buf_cap[i]) {
				size_t new_cap = buf_cap[i] == 0 ? 1024 : buf_cap[i] * 2;
				while (new_cap < buf_len[i] + (size_t)r + 1) new_cap *= 2;
				char* nb = (char*)realloc(buffers[i], new_cap);
				if (!nb) {
					fprintf(stderr, "Out of memory\n");
					close(fd[i][0]);
					closed[i] = true;
					open_pipes--;
					continue;
				}
				buffers[i] = nb;
				buf_cap[i] = new_cap;
			}
			memcpy(buffers[i] + buf_len[i], tmp, (size_t)r);
			buf_len[i] += (size_t)r;
			buffers[i][buf_len[i]] = '\0';

			char* start = buffers[i];
			char* nl = NULL;
			while ((nl = memchr(start, '\n', buf_len[i] - (size_t)(start - buffers[i])))) {
				*nl = '\0';
				char* word = NULL;
				int file_idx = 0;
				int line_no = 0;
				if (parse_record_line(start, &word, &file_idx, &line_no) == 0) {
					pipe_push(&all, &all_size, &all_capacity, word, file_idx, line_no);
				}
				free(word);
				start = nl + 1;
			}

			size_t remaining = buf_len[i] - (size_t)(start - buffers[i]);
			if (remaining > 0 && start != buffers[i]) {
				memmove(buffers[i], start, remaining);
			}
			buf_len[i] = remaining;
			buffers[i][buf_len[i]] = '\0';
		}
	}

	for (int i = 0; i < n; i++) {
		free(buffers[i]);
	}
	free(buffers);
	free(buf_len);
	free(buf_cap);
	free(closed);

	if (all_size > 1) {
		qsort(all, (size_t)all_size, sizeof(Occurrence), cmp_occurrence);
	}

	FILE* out = fopen(outputFileName, "w");
	if (!out) {
		fprintf(stderr, "Failed to open output file\n");
		free_occurrences(all, all_size);
		return 1;
	}

	int i = 0;
	while (i < all_size) {
		int j = i + 1;
		while (j < all_size && strcmp(all[i].word, all[j].word) == 0) {
			j++;
		}
		int count = j - i;
		fprintf(out, "%s (count=%d): ", all[i].word, count);
		for (int k = i; k < j; k++) {
			fprintf(out, "%d-%d", all[k].fileIndex, all[k].lineNo);
			if (k + 1 < j) {
				fprintf(out, ", ");
			}
		}
		fprintf(out, "\n");
		i = j;
	}
	fclose(out);
	free_occurrences(all, all_size);

    for(int i = 0; i < n; i++){
      waitpid(pids[i], NULL, 0);
    }

  	return 0;
}
