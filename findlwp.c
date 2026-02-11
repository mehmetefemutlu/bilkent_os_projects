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
#define R 0
#define W 1


typedef struct {
	int fileIndex;
	int lineNo;
	char* word;
} Occurences;

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

		if(w < 0){
			if (errno == EINTR) continue;
      			return -1;
		}

    		off += (size_t)w_data;
  	}
  	return 0;
}

static void pipe_push(Occurences** arr, int* size, int* capacity, const char* word, int fileIndex, int lineNo){
	if (*size == *capacity) {
    		int newCapacity = (*capacity == 0) ? 256 : (*capacity * 2);
  		Occurences *temp = (Occurences*)realloc(*arr, (size_t)newCapacity * sizeof(Occurences));
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


static void child_process(const char* filePrefix, int n, int k, int fileIndex, int dataLength, int writefd){
	char filename[512];
	makeInputFilename(filename, sizeof(filename), filePrefix, fileIndex);

	FILE* f = fopen(filename, "r");

	if(!f){
		close(writefd);
		_exit(2);
	}

	Occurences* o = NULL;
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
    		char rec[2048];
    		int m = snprintf(rec, sizeof(rec), "%s %d %d\n", o[i].word, o[i].fileIndex, o[i].lineNo);
    		if (m > 0) {
      			writeToParent(writefd, rec, (size_t)m, dataLength);
    		}
  	}
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
        else{
          printf("Pipe created succesfully\n");
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
        printf("Child created succesfully\n");

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

	//after this point to child deletion, parent must read data from pipes, sort it and process it.
	//child process and writing into pipes are done.

    for(int i = 0; i < n; i++){
      waitpid(pids[i], NULL, 0);
    }


	//for here, output file must be written out.
  	return 0;
}
