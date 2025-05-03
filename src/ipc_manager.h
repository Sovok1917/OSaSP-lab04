#ifndef IPC_MANAGER_H
#define IPC_MANAGER_H

#include "common.h"

// --- Function Declarations ---

// Initialization and Cleanup
int initialize_ipc(void);
void cleanup_resources(void); // Registered with atexit
void register_parent_signal_handlers(void);

// Process Management
int create_new_producer(void);
int create_new_consumer(void);
int stop_last_producer(void);
int stop_last_consumer(void);

// Status and Info
void display_status(void);
int get_producer_count(void);
int get_consumer_count(void);
unsigned long get_added_count(void);
unsigned long get_extracted_count(void);

// Semaphore Operations (used internally and by producer/consumer)
int semaphore_op(int sem_idx, int op);
int get_semaphore_id(void); // Needed by producer/consumer if not passed explicitly

// Shared Memory Access (used internally and by producer/consumer)
queue_t* get_shared_queue(void); // Needed by producer/consumer

#endif // IPC_MANAGER_H
