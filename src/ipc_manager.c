#include "ipc_manager.h"
#include "producer.h" // For producer_run prototype
#include "consumer.h" // For consumer_run prototype

// --- Static Global Variables (Module Scope) ---
static int s_semaphore_id = -1;         // Semaphore set ID
static int s_shared_memory_id = -1;     // Shared memory segment ID
static queue_t *s_shared_queue = NULL;  // Pointer to attached shared memory queue

static pid_t s_producer_pids[MAX_PRODUCERS];
static int s_producer_count = 0;

static pid_t s_consumer_pids[MAX_CONSUMERS];
static int s_consumer_count = 0;

// Global flag definition
volatile sig_atomic_t g_terminate_flag = 0;

// --- Static Function Declarations ---
static void remove_pid_from_list(pid_t pid_list[], int *count, pid_t pid_to_remove);
static void cleanup_processes(void);
static void parent_signal_handler(int sig);

// --- Union for semctl ---
// Definition required for semctl with SETALL or GETALL
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
    struct seminfo *__buf; // For Linux specific commands (if needed)
};


// --- Initialization and Cleanup ---

/*
 * initialize_ipc
 * Creates and initializes System V shared memory and semaphores.
 * Returns 0 on success, -1 on failure.
 */
int initialize_ipc(void) {
    // 1. Create Shared Memory
    s_shared_memory_id = shmget(IPC_PRIVATE, sizeof(queue_t), IPC_CREAT | IPC_EXCL | 0600);
    if (s_shared_memory_id == -1) {
        print_error("IPC Init", "shmget failed");
        return -1;
    }

    // 2. Attach Shared Memory
    s_shared_queue = (queue_t *)shmat(s_shared_memory_id, NULL, 0);
    if (s_shared_queue == (void *)-1) {
        print_error("IPC Init", "shmat failed");
        shmctl(s_shared_memory_id, IPC_RMID, NULL);
        s_shared_memory_id = -1;
        return -1;
    }

    // 3. Initialize Queue Structure
    s_shared_queue->head_idx = 0;
    s_shared_queue->tail_idx = 0;
    s_shared_queue->free_slots = QUEUE_CAPACITY;
    s_shared_queue->added_count = 0;
    s_shared_queue->extracted_count = 0;
    memset(s_shared_queue->messages, 0, sizeof(s_shared_queue->messages));

    // 4. Create Semaphores
    s_semaphore_id = semget(IPC_PRIVATE, NUM_SEMAPHORES, IPC_CREAT | IPC_EXCL | 0600);
    if (s_semaphore_id == -1) {
        print_error("IPC Init", "semget failed");
        shmdt(s_shared_queue);
        shmctl(s_shared_memory_id, IPC_RMID, NULL);
        s_shared_queue = NULL;
        s_shared_memory_id = -1;
        return -1;
    }

    // 5. Initialize Semaphores
    unsigned short sem_values[NUM_SEMAPHORES];
    sem_values[SEM_MUTEX_IDX] = 1;
    sem_values[SEM_EMPTY_IDX] = QUEUE_CAPACITY;
    sem_values[SEM_FULL_IDX] = 0;

    union semun arg; // Use the defined union
    arg.array = sem_values;

    if (semctl(s_semaphore_id, 0, SETALL, arg) == -1) {
        print_error("IPC Init", "semctl SETALL failed");
        semctl(s_semaphore_id, 0, IPC_RMID);
        shmdt(s_shared_queue);
        shmctl(s_shared_memory_id, IPC_RMID, NULL);
        s_semaphore_id = -1;
        s_shared_queue = NULL;
        s_shared_memory_id = -1;
        return -1;
    }

    print_info("IPC Init", "Shared memory and semaphores initialized successfully.");
    return 0;
}

/*
 * cleanup_resources
 * Registered with atexit. Cleans up IPC resources and child processes.
 */
void cleanup_resources(void) {
    print_info("Cleanup", "Starting resource cleanup...");
    restore_terminal(); // Restore terminal settings if modified

    // Signal any remaining children to terminate and wait for them
    cleanup_processes();

    // Detach Shared Memory
    if (s_shared_queue != NULL) {
        if (shmdt(s_shared_queue) == -1) {
            print_error("Cleanup", "shmdt failed");
        }
        s_shared_queue = NULL;
    }

    // Remove Shared Memory Segment (only if ID is valid)
    if (s_shared_memory_id != -1) {
        if (shmctl(s_shared_memory_id, IPC_RMID, NULL) == -1) {
            // Check errno - IPC_RMID fails if already removed or invalid ID
            if (errno != EINVAL && errno != EIDRM) {
                print_error("Cleanup", "shmctl IPC_RMID failed");
            }
        }
        s_shared_memory_id = -1;
    }

    // Remove Semaphores (only if ID is valid)
    if (s_semaphore_id != -1) {
        if (semctl(s_semaphore_id, 0, IPC_RMID) == -1) {
            if (errno != EINVAL && errno != EIDRM) {
                print_error("Cleanup", "semctl IPC_RMID failed");
            }
        }
        s_semaphore_id = -1;
    }

    print_info("Cleanup", "Resource cleanup complete.");
}

/*
 * parent_signal_handler
 * Handles SIGINT/SIGTERM in the parent process by setting the termination flag.
 */
static void parent_signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_terminate_flag = 1;
        // Avoid printf/fprintf in signal handlers
        // Use write for async-signal safety if message needed
        const char msg[] = "\n[Parent] Termination signal received. Shutting down...\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
}

/*
 * register_parent_signal_handlers
 * Sets up signal handlers for the parent process.
 */
void register_parent_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = parent_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // No SA_RESTART

    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        print_error("Signal", "Failed to register parent signal handlers");
        exit(EXIT_FAILURE);
    }
}

// --- Process Management ---

/*
 * remove_pid_from_list
 * Helper to remove a PID from a static PID array.
 */
static void remove_pid_from_list(pid_t pid_list[], int *count, pid_t pid_to_remove) {
    int i, found_idx = -1;
    for (i = 0; i < *count; ++i) {
        if (pid_list[i] == pid_to_remove) {
            found_idx = i;
            break;
        }
    }

    if (found_idx != -1) {
        // Shift elements down (memmove is safer but loop is fine here)
        for (i = found_idx; i < (*count - 1); ++i) {
            pid_list[i] = pid_list[i + 1];
        }
        (*count)--;
        // Optional: Zero out the now unused last slot
        // if (*count >= 0) pid_list[*count] = 0;
    }
}

/*
 * create_new_producer
 * Forks a new producer process.
 * Returns 0 on success, -1 on failure.
 */
int create_new_producer(void) {
    if (s_producer_count >= MAX_PRODUCERS) {
        print_info("Producer", "Maximum producer count reached.");
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        print_error("Producer", "fork failed");
        return -1;
    } else if (pid == 0) {
        // --- Child (Producer) Process ---
        signal(SIGINT, SIG_DFL); // Reset parent handlers
        signal(SIGTERM, SIG_DFL);
        // Call producer_run WITHOUT sem_id
        producer_run(s_producer_count + 1, s_shared_queue); // Pass ID starting from 1
        exit(EXIT_FAILURE); // Should not be reached
    } else {
        // --- Parent Process ---
        s_producer_pids[s_producer_count++] = pid;
        printf("[Parent] Created Producer %d (PID: %d)\r\n", s_producer_count, pid);
        fflush(stdout);
        return 0;
    }
}

/*
 * create_new_consumer
 * Forks a new consumer process.
 * Returns 0 on success, -1 on failure.
 */
int create_new_consumer(void) {
    if (s_consumer_count >= MAX_CONSUMERS) {
        print_info("Consumer", "Maximum consumer count reached.");
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        print_error("Consumer", "fork failed");
        return -1;
    } else if (pid == 0) {
        // --- Child (Consumer) Process ---
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        // Call consumer_run WITHOUT sem_id
        consumer_run(s_consumer_count + 1, s_shared_queue); // Pass ID starting from 1
        exit(EXIT_FAILURE); // Should not be reached
    } else {
        // --- Parent Process ---
        s_consumer_pids[s_consumer_count++] = pid;
        printf("[Parent] Created Consumer %d (PID: %d)\r\n", s_consumer_count, pid);
        fflush(stdout);
        return 0;
    }
}

/*
 * stop_last_producer
 * Sends SIGTERM to the most recently created producer and waits for it.
 * Returns 0 on success, -1 if no producers or error.
 */
int stop_last_producer(void) {
    if (s_producer_count <= 0) {
        print_info("Producer", "No producers running.");
        return -1;
    }

    int target_idx = s_producer_count - 1;
    pid_t pid_to_stop = s_producer_pids[target_idx];
    int current_id = target_idx + 1; // User-friendly ID

    printf("[Parent] Stopping producer %d (PID: %d)...\r\n", current_id, pid_to_stop);
    fflush(stdout);

    int kill_status = 0; // 0=ok, 1=already gone, -1=error
    if (kill(pid_to_stop, SIGTERM) == -1) {
        if (errno == ESRCH) {
            print_info("Producer", "Process already exited.");
            kill_status = 1;
        } else {
            print_error("Producer", "kill(SIGTERM) failed");
            return -1; // Don't remove if kill failed unexpectedly
        }
    }

    // Wait only if kill was sent successfully
    if (kill_status == 0) {
        int status;
        pid_t result = waitpid(pid_to_stop, &status, 0);
        if (result == -1 && errno != ECHILD) {
            print_error("Producer", "waitpid failed");
            // Proceed with removal as kill was sent
        } else if (result == pid_to_stop) {
            print_info("Producer", "Process terminated.");
        }
    }

    // Remove from list if kill succeeded or process was already gone
    remove_pid_from_list(s_producer_pids, &s_producer_count, pid_to_stop);
    printf("[Parent] Producer %d (PID: %d) removed. Remaining: %d\r\n", current_id, pid_to_stop, s_producer_count);
    fflush(stdout);
    return 0;
}

/*
 * stop_last_consumer
 * Sends SIGTERM to the most recently created consumer and waits for it.
 * Returns 0 on success, -1 if no consumers or error.
 */
int stop_last_consumer(void) {
    if (s_consumer_count <= 0) {
        print_info("Consumer", "No consumers running.");
        return -1;
    }

    int target_idx = s_consumer_count - 1;
    pid_t pid_to_stop = s_consumer_pids[target_idx];
    int current_id = target_idx + 1;

    printf("[Parent] Stopping consumer %d (PID: %d)...\r\n", current_id, pid_to_stop);
    fflush(stdout);

    int kill_status = 0; // 0=ok, 1=already gone, -1=error
    if (kill(pid_to_stop, SIGTERM) == -1) {
        if (errno == ESRCH) {
            print_info("Consumer", "Process already exited.");
            kill_status = 1;
        } else {
            print_error("Consumer", "kill(SIGTERM) failed");
            return -1;
        }
    }

    if (kill_status == 0) {
        int status;
        pid_t result = waitpid(pid_to_stop, &status, 0);
        if (result == -1 && errno != ECHILD) {
            print_error("Consumer", "waitpid failed");
        } else if (result == pid_to_stop) {
            print_info("Consumer", "Process terminated.");
        }
    }

    remove_pid_from_list(s_consumer_pids, &s_consumer_count, pid_to_stop);
    printf("[Parent] Consumer %d (PID: %d) removed. Remaining: %d\r\n", current_id, pid_to_stop, s_consumer_count);
    fflush(stdout);
    return 0;
}

/*
 * cleanup_processes
 * Sends SIGTERM to all remaining child processes and waits for them.
 */
static void cleanup_processes(void) {
    int i;
    pid_t pid;
    int status;
    int initial_producers = s_producer_count;
    int initial_consumers = s_consumer_count;
    int children_to_reap = initial_producers + initial_consumers;

    if (children_to_reap == 0) {
        print_info("Cleanup", "No child processes to terminate.");
        return;
    }

    print_info("Cleanup", "Sending SIGTERM to remaining children...");

    // Send SIGTERM to all producers
    for (i = 0; i < initial_producers; ++i) {
        pid = s_producer_pids[i];
        if (kill(pid, SIGTERM) == -1 && errno != ESRCH) {
            fprintf(stderr, "Warning: Failed to send SIGTERM to producer PID %d: %s\r\n", pid, strerror(errno));
        }
    }
    // Send SIGTERM to all consumers
    for (i = 0; i < initial_consumers; ++i) {
        pid = s_consumer_pids[i];
        if (kill(pid, SIGTERM) == -1 && errno != ESRCH) {
            fprintf(stderr, "Warning: Failed to send SIGTERM to consumer PID %d: %s\r\n", pid, strerror(errno));
        }
    }

    // Wake up any potentially blocked children
    // Increment Empty/Full semaphores enough times for all potential waiters
    print_info("Cleanup", "Signaling semaphores to unblock children...");
    for(i = 0; i < children_to_reap; ++i) {
        semaphore_op(SEM_EMPTY_IDX, 1); // Allow producers to potentially exit wait
        semaphore_op(SEM_FULL_IDX, 1);  // Allow consumers to potentially exit wait
    }


    print_info("Cleanup", "Waiting for children to exit...");
    int reaped_count = 0;
    time_t start_wait = time(NULL);
    const int wait_timeout_secs = 5; // Max time to wait

    // Wait for all children using blocking waitpid initially
    while (reaped_count < children_to_reap) {
        pid = waitpid(-1, &status, 0); // Blocking wait for any child
        if (pid > 0) {
            reaped_count++;
            // Find and remove the reaped PID from the correct list (optional here, lists are cleared after)
            // printf("[Cleanup] Reaped PID %d (%d/%d)\r\n", pid, reaped_count, children_to_reap); // Debug
        } else {
            if (errno == ECHILD) {
                // No more children left to wait for (might happen if some exited before signal)
                print_info("Cleanup", "No more children found by waitpid.");
                break;
            } else if (errno == EINTR) {
                // Interrupted, maybe by our own signal handler? Continue loop.
                print_info("Cleanup", "waitpid interrupted, continuing...");
                continue;
            } else {
                // Unexpected error
                print_error("Cleanup", "waitpid failed");
                break; // Stop waiting on error
            }
        }
        // Timeout check
        if (time(NULL) - start_wait > wait_timeout_secs) {
            fprintf(stderr, "Warning: Timeout waiting for all children to exit.\r\n");
            break;
        }
    }

    // Ensure lists are cleared after waiting
    s_producer_count = 0;
    s_consumer_count = 0;
    print_info("Cleanup", "Finished waiting for children.");
}


// --- Status and Info ---

/*
 * display_status
 * Prints the current status of the queue and processes.
 */
void display_status(void) {
    // Lock mutex to get consistent queue state
    if (semaphore_op(SEM_MUTEX_IDX, -1) == -1) {
        // Check if termination is happening
        if (errno == ECANCELED || g_terminate_flag) return;
        print_error("Status", "Failed to lock mutex for status");
        return;
    }

    // Read queue state while holding mutex
    int capacity = QUEUE_CAPACITY;
    int free_slots = s_shared_queue->free_slots;
    int occupied = capacity - free_slots;
    unsigned long added = s_shared_queue->added_count;
    unsigned long extracted = s_shared_queue->extracted_count;

    // Unlock mutex
    if (semaphore_op(SEM_MUTEX_IDX, 1) == -1) {
        print_error("Status", "Failed to unlock mutex after status");
    }

    // Print status (using data read under lock)
    printf("\n--- System Status ---\r\n");
    printf("Queue Capacity: %d\r\n", capacity);
    printf("Queue Occupied: %d\r\n", occupied);
    printf("Queue Free:     %d\r\n", free_slots);
    printf("Total Added:    %lu\r\n", added);
    printf("Total Extracted:%lu\r\n", extracted);
    printf("Producers:      %d / %d\r\n", s_producer_count, MAX_PRODUCERS);
    printf("Consumers:      %d / %d\r\n", s_consumer_count, MAX_CONSUMERS);
    printf("---------------------\r\n");
    fflush(stdout);
}

int get_producer_count(void) { return s_producer_count; }
int get_consumer_count(void) { return s_consumer_count; }
unsigned long get_added_count(void) { return s_shared_queue ? s_shared_queue->added_count : 0; }
unsigned long get_extracted_count(void) { return s_shared_queue ? s_shared_queue->extracted_count : 0; }


// --- Semaphore Operations ---

/*
 * semaphore_op
 * Performs a semaphore operation (wait/signal). Handles EINTR.
 * Returns 0 on success, -1 on failure (sets errno).
 */
int semaphore_op(int sem_idx, int op) {
    if (s_semaphore_id == -1) {
        errno = EINVAL;
        // Avoid printing error here if called during cleanup when sem might be gone
        // print_error("Semaphore", "Semaphore set not initialized");
        return -1;
    }
    struct sembuf sb;
    sb.sem_num = (unsigned short)sem_idx;
    sb.sem_op = (short)op;
    sb.sem_flg = 0;

    while (semop(s_semaphore_id, &sb, 1) == -1) {
        if (errno == EINTR) {
            // Check global termination flag ONLY if parent is calling
            // Children check their own flag after semop returns EINTR
            // For simplicity here, let's assume parent might call this too
            if (g_terminate_flag) {
                errno = ECANCELED; // Indicate cancellation
                return -1;
            }
            continue; // Retry if not terminating
        } else {
            // Avoid printing error if sem ID removed during cleanup (EIDRM, EINVAL)
            if (errno != EIDRM && errno != EINVAL) {
                char msg[100];
                snprintf(msg, sizeof(msg), "semop failed for sem_idx %d, op %d", sem_idx, op);
                print_error("Semaphore", msg);
            }
            return -1; // Return error
        }
    }
    return 0;
}

// --- Shared Memory Access ---

/*
 * get_shared_queue
 * Returns a pointer to the shared queue structure.
 */
queue_t* get_shared_queue(void) {
    return s_shared_queue;
}
