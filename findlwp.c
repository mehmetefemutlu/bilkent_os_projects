#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#define R 0
#define W 1

static void message(const char* program){
  fprintf(stderr, "Usage: %s FilePrefix N K DataLen OutFilename\n", program, program);
};

static void makeInputFilename(char* buffer, size_t buffersize, const char* prefix, int i) {
  int n = snprintf(buffer, buffersize, "%s%d", prefix, i);
  if (n < 0 || (size_t)n >= buffersize) {
    fprintf(stderr, "Filename too long\n");
    exit(1);
  }
};


static void child_process(const char* filePrefix, int n, int k, int fileIndex, int dataLength, int writefd){
//to be implemented
};

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
  	if(pipe(fd[i] < 0)) {
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
	}

	//parent process
	pids[i] = pid;
	close(fd[i][1]);
  }
  return 0;
}  
