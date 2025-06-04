// Purpose: Manages IPC resources (shared memory, semaphores) and child processes.
#include "ipc_manager.h"
#include "producer.h"
#include "consumer.h"
#include "utils.h"

// Static global variables for IPC identifiers and process tracking
static int s_semaphore_id = -1;
static int s_shared_memory_id = -1;
static queue_t *s_shared_queue = NULL;

static pid_t s_producer_pids[MAX_PRODUCERS];
static int s_producer_count = 0;

static pid_t s_consumer_pids[MAX_CONSUMERS];
static int s_consumer_count = 0;

// Global termination flag, set by parent's signal handler
volatile sig_atomic_t g_terminate_flag = 0;

// Forward declarations for static helper functions
static void remove_pid_from_list(pid_t pid_list[], int *count, pid_t pid_to_remove);
static void cleanup_processes(void);
static void parent_signal_handler(int sig);

// Union for semctl calls
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
    #if defined(__linux__) || defined(__GNU_LIBRARY__) // For Linux/glibc
    struct seminfo *__buf; // Used for IPC_INFO
    #endif
};


/*
 * initialize_ipc
 * Initializes System V shared memory and semaphores.
 * Sets up the shared queue structure and semaphore values.
 * Parameters:
 *   None.
 * Returns:
 *   0 on success, -1 on failure.
 */
int initialize_ipc(void) {
    // Initialize shared memory
    s_shared_memory_id = shmget(IPC_PRIVATE, sizeof(queue_t), IPC_CREAT | IPC_EXCL | 0600);
    if (s_shared_memory_id == -1) {
        print_error("IPC Init", "shmget failed");
        return -1;
    }

    s_shared_queue = (queue_t *)shmat(s_shared_memory_id, NULL, 0);
    if (s_shared_queue == (void *)-1) {
        print_error("IPC Init", "shmat failed");
        shmctl(s_shared_memory_id, IPC_RMID, NULL); // Clean up shm if shmat fails
        s_shared_memory_id = -1;
        s_shared_queue = NULL; // Ensure pointer is NULL after failure
        return -1;
    }

    // Initialize queue structure in shared memory
    s_shared_queue->head_idx = 0;
    s_shared_queue->tail_idx = 0;
    s_shared_queue->free_slots = QUEUE_CAPACITY;
    s_shared_queue->added_count = 0;
    s_shared_queue->extracted_count = 0;
    memset(s_shared_queue->messages, 0, sizeof(s_shared_queue->messages));

    // Initialize semaphores
    s_semaphore_id = semget(IPC_PRIVATE, NUM_SEMAPHORES, IPC_CREAT | IPC_EXCL | 0600);
    if (s_semaphore_id == -1) {
        print_error("IPC Init", "semget failed");
        shmdt(s_shared_queue);
        shmctl(s_shared_memory_id, IPC_RMID, NULL);
        s_shared_queue = NULL;
        s_shared_memory_id = -1;
        return -1;
    }

    // Set initial values for semaphores
    unsigned short sem_values[NUM_SEMAPHORES];
    sem_values[SEM_MUTEX_IDX] = 1;            // Mutex: available
    sem_values[SEM_EMPTY_IDX] = QUEUE_CAPACITY; // Empty slots: queue capacity
    sem_values[SEM_FULL_IDX] = 0;             // Full slots: 0 initially

    union semun arg;
    arg.array = sem_values;

    if (semctl(s_semaphore_id, 0, SETALL, arg) == -1) {
        print_error("IPC Init", "semctl SETALL failed");
        semctl(s_semaphore_id, 0, IPC_RMID); // Clean up semaphores
        shmdt(s_shared_queue);
        shmctl(s_shared_memory_id, IPC_RMID, NULL); // Clean up shared memory
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
 * Cleans up all IPC resources (shared memory, semaphores) and terminates child processes.
 * This function is typically registered with atexit().
 * Parameters:
 *   None.
 * Returns:
 *   None.
 */
void cleanup_resources(void) {
    print_info("Cleanup", "Starting resource cleanup...");
    restore_terminal(); // Restore terminal settings if modified

    cleanup_processes(); // Terminate and reap child processes

    // Detach shared memory
    if (s_shared_queue != NULL) {
        if (shmdt(s_shared_queue) == -1) {
            // EIDRM means it was already removed, EINVAL means invalid shmid (possibly already detached/removed)
            if (errno != EIDRM && errno != EINVAL) {
                print_error("Cleanup", "shmdt failed");
            }
        }
        s_shared_queue = NULL; // Mark as detached
    }

    // Remove shared memory segment
    if (s_shared_memory_id != -1) {
        if (shmctl(s_shared_memory_id, IPC_RMID, NULL) == -1) {
            // EINVAL or EIDRM means it's already gone or was never valid, not an error in cleanup.
            if (errno != EINVAL && errno != EIDRM) {
                print_error("Cleanup", "shmctl IPC_RMID failed");
            }
        }
        s_shared_memory_id = -1; // Mark as removed
    }

    // Remove semaphore set
    if (s_semaphore_id != -1) {
        if (semctl(s_semaphore_id, 0, IPC_RMID) == -1) {
            // EINVAL or EIDRM means it's already gone or was never valid.
            if (errno != EINVAL && errno != EIDRM) {
                print_error("Cleanup", "semctl IPC_RMID failed");
            }
        }
        s_semaphore_id = -1; // Mark as removed
    }

    print_info("Cleanup", "Resource cleanup complete.");
}

/*
 * parent_signal_handler
 * Signal handler for the parent process (main). Sets the global termination flag.
 * Parameters:
 *   sig - The signal number received (SIGINT or SIGTERM).
 * Returns:
 *   None.
 */
static void parent_signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_terminate_flag = 1;
        // Use write for async-signal safety
        const char msg[] = "\n[Parent] Termination signal received. Shutting down...\n";
        ssize_t bytes_written = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)bytes_written; // Suppress unused result warning
    }
}

/*
 * register_parent_signal_handlers
 * Registers signal handlers for SIGINT and SIGTERM in the parent process.
 * Parameters:
 *   None.
 * Returns:
 *   None. Exits on failure to register handlers.
 */
void register_parent_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = parent_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // Consider SA_RESTART if appropriate for other syscalls

    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        print_error("Signal", "Failed to register parent signal handlers");
        // If this fails, cleanup might not be graceful. Exiting.
        exit(EXIT_FAILURE);
    }
}

/*
 * remove_pid_from_list
 * Helper function to remove a PID from a list of PIDs.
 * Parameters:
 *   pid_list        - Array of PIDs.
 *   count           - Pointer to the current number of PIDs in the list.
 *   pid_to_remove   - The PID to remove.
 * Returns:
 *   None. Modifies pid_list and count directly.
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
        // Shift elements to fill the gap
        for (i = found_idx; i < (*count - 1); ++i) {
            pid_list[i] = pid_list[i + 1];
        }
        (*count)--; // Decrement the count
    }
}

/*
 * create_new_producer
 * Forks a new producer process. The child process will execute producer_run().
 * Parameters:
 *   None.
 * Returns:
 *   0 on success (in parent), -1 on failure (e.g., max producers reached, fork failed).
 *   Child process does not return from this function (calls _exit).
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
    } else if (pid == 0) { // Child process
        // Reset signal handlers inherited from parent to default before child sets its own.
        // This is good practice, though child's sigaction will override anyway.
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        // Child runs the producer logic
        producer_run(s_producer_count + 1, s_shared_queue); // ID is 1-based for display
        _exit(EXIT_FAILURE); // Should not be reached if producer_run exits normally
    } else { // Parent process
        s_producer_pids[s_producer_count++] = pid;
        printf("[Parent] Created Producer %d (PID: %d)\r\n", s_producer_count, pid);
        fflush(stdout);
        return 0;
    }
}

/*
 * create_new_consumer
 * Forks a new consumer process. The child process will execute consumer_run().
 * Parameters:
 *   None.
 * Returns:
 *   0 on success (in parent), -1 on failure.
 *   Child process does not return.
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
    } else if (pid == 0) { // Child process
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        consumer_run(s_consumer_count + 1, s_shared_queue); // ID is 1-based
        _exit(EXIT_FAILURE);
    } else { // Parent process
        s_consumer_pids[s_consumer_count++] = pid;
        printf("[Parent] Created Consumer %d (PID: %d)\r\n", s_consumer_count, pid);
        fflush(stdout);
        return 0;
    }
}

/*
 * stop_last_producer
 * Stops the most recently created producer process by sending SIGTERM and waiting for it.
 * Parameters:
 *   None.
 * Returns:
 *   0 on success or if process was already gone, -1 on failure to signal/wait.
 */
int stop_last_producer(void) {
    if (s_producer_count <= 0) {
        print_info("Producer", "No producers running.");
        return -1;
    }
    int target_idx = s_producer_count - 1; // Index of the last producer
    pid_t pid_to_stop = s_producer_pids[target_idx];
    // The "ID" for display was s_producer_count at creation time.
    // This can be confusing if producers are stopped out of order.
    // Using the PID is more robust for messages.
    printf("[Parent] Stopping producer (PID: %d)...\r\n", pid_to_stop);
    fflush(stdout);

    bool already_exited = false;
    if (kill(pid_to_stop, SIGTERM) == -1) {
        if (errno == ESRCH) { // Process already gone
            print_info("Producer", "Process already exited.");
            already_exited = true;
        } else {
            print_error("Producer", "kill(SIGTERM) failed");
            return -1; // Failed to signal, don't remove from list yet
        }
    }

    if (!already_exited) {
        int status;
        pid_t result;
        do {
            result = waitpid(pid_to_stop, &status, 0);
        } while (result == -1 && errno == EINTR && !g_terminate_flag); // Retry if EINTR, unless global termination

        if (result == pid_to_stop) {
            print_info("Producer", "Process terminated.");
        } else if (result == -1) {
            if (errno == ECHILD) {
                print_info("Producer", "Process already reaped (ECHILD).");
            } else if (!(errno == EINTR && g_terminate_flag)) {
                // Only print error if not interrupted due to global shutdown
                print_error("Producer", "waitpid failed");
                // Even if waitpid fails, we might still remove it if ESRCH occurred earlier
                // or if we assume it's gone. For now, if waitpid fails unexpectedly,
                // it's an error state.
            }
        }
    }
    // Remove from list if signaled, or if found to be already exited.
    remove_pid_from_list(s_producer_pids, &s_producer_count, pid_to_stop);
    printf("[Parent] Producer (PID: %d) removed. Remaining: %d\r\n", pid_to_stop, s_producer_count);
    fflush(stdout);
    return 0;
}

/*
 * stop_last_consumer
 * Stops the most recently created consumer process.
 * Parameters:
 *   None.
 * Returns:
 *   0 on success or if process was already gone, -1 on failure.
 */
int stop_last_consumer(void) {
    if (s_consumer_count <= 0) {
        print_info("Consumer", "No consumers running.");
        return -1;
    }
    int target_idx = s_consumer_count - 1;
    pid_t pid_to_stop = s_consumer_pids[target_idx];
    printf("[Parent] Stopping consumer (PID: %d)...\r\n", pid_to_stop);
    fflush(stdout);

    bool already_exited = false;
    if (kill(pid_to_stop, SIGTERM) == -1) {
        if (errno == ESRCH) {
            print_info("Consumer", "Process already exited.");
            already_exited = true;
        } else {
            print_error("Consumer", "kill(SIGTERM) failed");
            return -1;
        }
    }

    if (!already_exited) {
        int status;
        pid_t result;
        do {
            result = waitpid(pid_to_stop, &status, 0);
        } while (result == -1 && errno == EINTR && !g_terminate_flag);

        if (result == pid_to_stop) {
            print_info("Consumer", "Process terminated.");
        } else if (result == -1) {
            if (errno == ECHILD) {
                print_info("Consumer", "Process already reaped (ECHILD).");
            } else if (!(errno == EINTR && g_terminate_flag)) {
                print_error("Consumer", "waitpid failed");
            }
        }
    }
    remove_pid_from_list(s_consumer_pids, &s_consumer_count, pid_to_stop);
    printf("[Parent] Consumer (PID: %d) removed. Remaining: %d\r\n", pid_to_stop, s_consumer_count);
    fflush(stdout);
    return 0;
}

/*
 * cleanup_processes
 * Sends SIGTERM to all running child processes and waits for them to terminate.
 * Also signals semaphores to help unblock any children waiting on them.
 * Parameters:
 *   None.
 * Returns:
 *   None.
 */
static void cleanup_processes(void) {
    int i;
    pid_t pid;
    int status;
    // Make copies of counts, as the actual s_producer_count/s_consumer_count might be modified
    // by stop_last_X functions if called concurrently (though not expected during atexit).
    // Or, rely on the fact that this is usually called when no other operations are modifying these.
    int initial_producers = s_producer_count;
    int initial_consumers = s_consumer_count;
    int children_to_reap = initial_producers + initial_consumers;

    if (children_to_reap == 0) {
        print_info("Cleanup", "No child processes to terminate.");
        return;
    }

    print_info("Cleanup", "Sending SIGTERM to remaining children...");
    for (i = 0; i < initial_producers; ++i) {
        pid = s_producer_pids[i];
        if (kill(pid, SIGTERM) == -1 && errno != ESRCH) {
            fprintf(stderr, "Warning: Failed to send SIGTERM to producer PID %d: %s\r\n", pid, strerror(errno));
        }
    }
    for (i = 0; i < initial_consumers; ++i) {
        pid = s_consumer_pids[i];
        if (kill(pid, SIGTERM) == -1 && errno != ESRCH) {
            fprintf(stderr, "Warning: Failed to send SIGTERM to consumer PID %d: %s\r\n", pid, strerror(errno));
        }
    }

    // Signal semaphores to unblock any children potentially stuck waiting.
    // This helps them receive and process the SIGTERM/SIGINT.
    print_info("Cleanup", "Signaling semaphores to unblock children...");
    for(i = 0; i < children_to_reap * 2 ; ++i) { // Signal more than enough times
        semaphore_op(SEM_EMPTY_IDX, 1); // Allow producers to proceed if stuck on empty
        semaphore_op(SEM_FULL_IDX, 1);  // Allow consumers to proceed if stuck on full
    }
    // Also signal mutex in case a child holds it and got stuck before releasing
    // semaphore_op(SEM_MUTEX_IDX, 1); // This is risky if no one holds it, could increment beyond 1.
    // Better to rely on children's signal handlers.

    print_info("Cleanup", "Waiting for children to exit...");
    int reaped_count = 0;
    time_t start_wait = time(NULL);
    const int wait_timeout_secs = 5; // Max time to wait for children

    // Loop to reap children
    while (reaped_count < children_to_reap) {
        pid = waitpid(-1, &status, 0); // Blocking wait for any child
        if (pid > 0) {
            reaped_count++;
            // Optionally, identify which child was reaped and remove from s_producer_pids/s_consumer_pids
            // For simplicity, we just decrement counts after the loop.
        } else { // waitpid error
            if (errno == ECHILD) { // No more children
                print_info("Cleanup", "No more children found by waitpid (ECHILD).");
                break;
            } else if (errno == EINTR) { // Interrupted by a signal
                print_info("Cleanup", "waitpid interrupted, continuing...");
                // If g_terminate_flag is set, we are already in shutdown.
                continue;
            } else { // Other waitpid error
                print_error("Cleanup", "waitpid failed");
                break; // Stop trying to reap on other errors
            }
        }
        // Check for timeout within the loop if waitpid could block indefinitely
        // For blocking waitpid, this check is effectively after each child is reaped or error.
        if (time(NULL) - start_wait > wait_timeout_secs && reaped_count < children_to_reap) {
            fprintf(stderr, "Warning: Timeout waiting for all children to exit. %d of %d reaped.\r\n", reaped_count, children_to_reap);
            // Consider sending SIGKILL to remaining children if a hard stop is needed.
            break;
        }
    }
    // Reset counts after attempting to reap all.
    s_producer_count = 0;
    s_consumer_count = 0;
    print_info("Cleanup", "Finished waiting for children.");
}

/*
 * display_status
 * Prints the current status of the shared queue and process counts.
 * Acquires and releases the mutex semaphore to safely read queue statistics.
 * Parameters:
 *   None.
 * Returns:
 *   None.
 */
void display_status(void) {
    // Attempt to lock mutex to read shared queue state
    if (semaphore_op(SEM_MUTEX_IDX, -1) == -1) {
        // If terminating, or IPC is gone, don't try to read status.
        if (g_terminate_flag || errno == EINVAL || errno == EIDRM || errno == ECANCELED) return;
        print_error("Status", "Failed to lock mutex for status");
        return;
    }

    // Read shared data under mutex protection
    int capacity = QUEUE_CAPACITY;
    int free_slots = s_shared_queue->free_slots; // Assuming s_shared_queue is valid
    int occupied = capacity - free_slots;
    unsigned long added = s_shared_queue->added_count;
    unsigned long extracted = s_shared_queue->extracted_count;

    // Release mutex
    if (semaphore_op(SEM_MUTEX_IDX, 1) == -1) {
        // If IPC is gone, this might fail.
        if (errno != EINVAL && errno != EIDRM) {
            print_error("Status", "Failed to unlock mutex after status");
        }
    }

    // Print status information
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

// Getter functions for process counts and queue statistics
int get_producer_count(void) { return s_producer_count; }
int get_consumer_count(void) { return s_consumer_count; }
// Accessing s_shared_queue directly here is not thread/IPC-safe without mutex.
// These are primarily for main loop's indicative display, not for critical logic.
// For accurate counts, mutex should be held, or values copied under mutex.
unsigned long get_added_count(void) { return (s_shared_queue != NULL) ? s_shared_queue->added_count : 0; }
unsigned long get_extracted_count(void) { return (s_shared_queue != NULL) ? s_shared_queue->extracted_count : 0; }


/*
 * semaphore_op
 * Performs a specified operation on a semaphore in the set.
 * This is a wrapper around semop().
 * Parameters:
 *   sem_idx - Index of the semaphore in the set.
 *   op      - Operation to perform (e.g., -1 for wait, 1 for signal).
 * Returns:
 *   0 on success, -1 on failure (errno is set by semop).
 *   Does NOT automatically retry on EINTR; caller must handle.
 */
int semaphore_op(int sem_idx, int op) {
    if (s_semaphore_id == -1) { // Check if semaphore system is initialized
        errno = EINVAL; // Or some other appropriate error
        return -1;
    }
    struct sembuf sb;
    sb.sem_num = (unsigned short)sem_idx; // Semaphore number in the set
    sb.sem_op = (short)op;                // Operation value
    sb.sem_flg = 0;                       // No flags (e.g., SEM_UNDO, IPC_NOWAIT)

    if (semop(s_semaphore_id, &sb, 1) == -1) {
        // Avoid printing errors for expected conditions during shutdown or if IPC is gone,
        // or if interrupted by a signal (EINTR), which the caller should handle.
        if (errno != EIDRM && errno != EINVAL && errno != EINTR && errno != ECANCELED) {
            char msg[100];
            snprintf(msg, sizeof(msg), "semop failed for sem_idx %d, op %d", sem_idx, op);
            // This print_error can be noisy if semaphores are removed while ops are pending.
            // print_error("Semaphore", msg);
        }
        return -1; // Return -1, errno is set by semop
    }
    return 0; // Success
}

/*
 * get_shared_queue
 * Returns a pointer to the shared queue.
 * Parameters:
 *   None.
 * Returns:
 *   Pointer to queue_t, or NULL if not initialized.
 */
queue_t* get_shared_queue(void) {
    return s_shared_queue;
}
