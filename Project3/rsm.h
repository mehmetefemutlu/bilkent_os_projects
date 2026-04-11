#ifndef RSM_H
#define RSM_H

#define MAX_RT 100
#define MAX_PR 100

int rsm_init(int p_count, int r_count, int exist[], int avoid);
int rsm_destroy(void);
int rsm_process_started(int apid);
int rsm_process_ended(void);
int rsm_claim(int claim[]);
int rsm_request(int request[]);
int rsm_release(int release[]);
int rsm_detection(void);
void rsm_print_state(char headermsg[]);

#endif
