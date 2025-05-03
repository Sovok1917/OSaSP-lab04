#ifndef IPC_MANAGER_H
#define IPC_MANAGER_H

#include "common.h"




int initialize_ipc(void);
void cleanup_resources(void);
void register_parent_signal_handlers(void);


int create_new_producer(void);
int create_new_consumer(void);
int stop_last_producer(void);
int stop_last_consumer(void);


void display_status(void);
int get_producer_count(void);
int get_consumer_count(void);
unsigned long get_added_count(void);
unsigned long get_extracted_count(void);


int semaphore_op(int sem_idx, int op);
int get_semaphore_id(void);


queue_t* get_shared_queue(void);

#endif
