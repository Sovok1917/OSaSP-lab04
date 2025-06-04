// Purpose: Implements the producer process logic.
#include "producer.h"
#include "ipc_manager.h"
#include "utils.h"

// Child-local flag to signal termination.
static volatile sig_atomic_t s_child_terminate_flag = 0;

/*
 * child_signal_handler
 * Sets the child-local termination flag upon receiving SIGTERM or SIGINT.
 * Parameters:
 *   sig - The signal number received.
 * Returns:
 *   None.
 */
static void child_signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        s_child_terminate_flag = 1;
        // Using write() as it's async-signal-safe.
        const char msg[] = "[Producer] Termination signal received, shutting down...\n";
        ssize_t bytes_written = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        // Avoid -Wunused-result
        (void)bytes_written;
    }
}

/*
 * producer_run
 * Main function for a producer process.
 * Generates messages, adds them to the shared queue using semaphores for synchronization.
 * It sets up signal handlers for SIGTERM and SIGINT to enable graceful shutdown.
 * The producer loop continues until s_child_terminate_flag is set.
 * On termination, it attempts to print a final message and exits.
 * Parameters:
 *   producer_id - An identifier for this producer instance.
 *   queue       - Pointer to the shared memory queue.
 * Returns:
 *   Does not return; calls _exit().
 */
void producer_run(int producer_id, queue_t *queue) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = child_signal_handler;
    sigemptyset(&sa.sa_mask); // Important to initialize the mask
    sa.sa_flags = 0; // No special flags needed here (e.g., SA_RESTART)

    // Register handlers for SIGTERM and SIGINT
    if (sigaction(SIGTERM, &sa, NULL) == -1 || sigaction(SIGINT, &sa, NULL) == -1) {
        print_error("Producer", "Failed to register signal handlers");
        _exit(EXIT_FAILURE); // Use _exit in child processes
    }

    // Seed random number generator uniquely for this process
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    char info_prefix[32];
    snprintf(info_prefix, sizeof(info_prefix), "Producer %d", producer_id);
    print_info(info_prefix, "Started.");

    while (!s_child_terminate_flag) {
        message_t msg;

        // Generate a new message
        msg.type = (unsigned char)(rand() % 256);
        // Ensure size is at least 0 and less than MAX_DATA_SIZE
        msg.size = (unsigned char)(rand() % MAX_DATA_SIZE);
        for (int i = 0; i < msg.size; ++i) {
            msg.data[i] = (unsigned char)(rand() % 256);
        }
        msg.hash = 0; // Zero out hash before calculation
        msg.hash = calculate_message_hash(&msg);

        // Wait for an empty slot in the queue
        while (semaphore_op(SEM_EMPTY_IDX, -1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during wait for empty slot (EINTR).");
                goto cleanup_and_exit;
            } else if (errno == EINTR) { // Interrupted by a different signal, retry
                continue;
            } else { // Other semaphore error
                print_error(info_prefix, "Semaphore wait (Empty) failed.");
                goto cleanup_and_exit;
            }
        }

        // Check termination flag again after potentially blocking call
        if (s_child_terminate_flag) {
            semaphore_op(SEM_EMPTY_IDX, 1); // Release semaphore if acquired and now terminating
            print_info(info_prefix, "Terminating after wait for empty slot.");
            break;
        }

        // Wait for mutex to access the queue
        while (semaphore_op(SEM_MUTEX_IDX, -1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                semaphore_op(SEM_EMPTY_IDX, 1); // Release previously acquired SEM_EMPTY
                print_info(info_prefix, "Terminating during wait for mutex (EINTR).");
                goto cleanup_and_exit;
            } else if (errno == EINTR) {
                continue;
            } else {
                semaphore_op(SEM_EMPTY_IDX, 1); // Release previously acquired SEM_EMPTY
                print_error(info_prefix, "Semaphore wait (Mutex) failed.");
                goto cleanup_and_exit;
            }
        }

        if (s_child_terminate_flag) {
            semaphore_op(SEM_MUTEX_IDX, 1); // Release mutex
            semaphore_op(SEM_EMPTY_IDX, 1); // Release SEM_EMPTY
            print_info(info_prefix, "Terminating after wait for mutex.");
            break;
        }

        // Critical section: Add message to queue
        memcpy(&queue->messages[queue->tail_idx], &msg, sizeof(message_t));
        queue->tail_idx = (queue->tail_idx + 1) % QUEUE_CAPACITY;
        queue->free_slots--;
        queue->added_count++;
        unsigned long current_added = queue->added_count;

        // Release mutex
        if (semaphore_op(SEM_MUTEX_IDX, 1) == -1) {
            // This is a critical failure, as mutex might be left locked.
            print_error(info_prefix, "CRITICAL: Failed to release mutex!");
            // Consider how to handle this; exiting might be the safest.
            goto cleanup_and_exit;
        }

        // Signal that a slot is full
        if (semaphore_op(SEM_FULL_IDX, 1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during signal full (EINTR).");
                // The message is in the queue, but consumer might not be signaled if this fails.
                // However, if terminating, this might be acceptable.
                break;
            } else if (errno != EINTR) { // Other semaphore error
                print_error(info_prefix, "Semaphore signal (Full) failed.");
                // If this fails, consumers might not wake up.
                break; // Exit loop
            }
            // If EINTR and not terminating, loop might retry if applicable, but sem_op is not designed for retry here.
        }

        printf("[%s] Added msg (Type:%u Size:%u Hash:%u). Total Added: %lu\r\n",
               info_prefix, msg.type, msg.size, msg.hash, current_added);
        fflush(stdout);

        // Simulate work with a random delay
        struct timespec delay_req, delay_rem;
        long delay_us = (rand() % 400000L) + 100000L; // 0.1 to 0.5 seconds
        delay_req.tv_sec = delay_us / 1000000L;
        delay_req.tv_nsec = (delay_us % 1000000L) * 1000L;
        while (nanosleep(&delay_req, &delay_rem) == -1) {
            if (errno == EINTR) {
                if (s_child_terminate_flag) break; // Exit sleep if terminating
                delay_req = delay_rem; // Resume sleep with remaining time
            } else {
                print_error(info_prefix, "nanosleep failed");
                break; // Exit sleep loop on other errors
            }
        }
    }

    cleanup_and_exit:
    print_info(info_prefix, "Terminating gracefully...");
    fflush(stdout); // Attempt to flush stdio buffers
    fflush(stderr);
    _exit(EXIT_SUCCESS); // Use _exit for child processes
}
