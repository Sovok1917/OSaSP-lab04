// Purpose: Implements the consumer process logic.
#include "consumer.h"
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
        const char msg[] = "[Consumer] Termination signal received, shutting down...\n";
        ssize_t bytes_written = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        // Avoid -Wunused-result
        (void)bytes_written;
    }
}

/*
 * consumer_run
 * Main function for a consumer process.
 * Retrieves messages from the shared queue using semaphores for synchronization,
 * verifies message hash, and prints status.
 * It sets up signal handlers for SIGTERM and SIGINT to enable graceful shutdown.
 * The consumer loop continues until s_child_terminate_flag is set.
 * On termination, it attempts to print a final message and exits.
 * Parameters:
 *   consumer_id - An identifier for this consumer instance.
 *   queue       - Pointer to the shared memory queue.
 * Returns:
 *   Does not return; calls _exit().
 */
void consumer_run(int consumer_id, queue_t *queue) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = child_signal_handler;
    sigemptyset(&sa.sa_mask); // Important to initialize the mask
    sa.sa_flags = 0; // No special flags needed

    // Register handlers for SIGTERM and SIGINT
    if (sigaction(SIGTERM, &sa, NULL) == -1 || sigaction(SIGINT, &sa, NULL) == -1) {
        print_error("Consumer", "Failed to register signal handlers");
        _exit(EXIT_FAILURE); // Use _exit in child processes
    }

    // Seed random number generator uniquely for this process
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    char info_prefix[32];
    snprintf(info_prefix, sizeof(info_prefix), "Consumer %d", consumer_id);
    print_info(info_prefix, "Started.");

    while (!s_child_terminate_flag) {
        message_t msg;
        unsigned short original_hash;
        unsigned short calculated_hash;
        unsigned long current_extracted;

        // Wait for a full slot in the queue
        while (semaphore_op(SEM_FULL_IDX, -1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during wait for full slot (EINTR).");
                goto cleanup_and_exit;
            } else if (errno == EINTR) { // Interrupted by a different signal, retry
                continue;
            } else { // Other semaphore error
                print_error(info_prefix, "Semaphore wait (Full) failed.");
                goto cleanup_and_exit;
            }
        }

        // Check termination flag again after potentially blocking call
        if (s_child_terminate_flag) {
            semaphore_op(SEM_FULL_IDX, 1); // Release semaphore if acquired and now terminating
            print_info(info_prefix, "Terminating after wait for full slot.");
            break;
        }

        // Wait for mutex to access the queue
        while (semaphore_op(SEM_MUTEX_IDX, -1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                semaphore_op(SEM_FULL_IDX, 1); // Release previously acquired SEM_FULL
                print_info(info_prefix, "Terminating during wait for mutex (EINTR).");
                goto cleanup_and_exit;
            } else if (errno == EINTR) {
                continue;
            } else {
                semaphore_op(SEM_FULL_IDX, 1); // Release previously acquired SEM_FULL
                print_error(info_prefix, "Semaphore wait (Mutex) failed.");
                goto cleanup_and_exit;
            }
        }

        if (s_child_terminate_flag) {
            semaphore_op(SEM_MUTEX_IDX, 1); // Release mutex
            semaphore_op(SEM_FULL_IDX, 1); // Release SEM_FULL
            print_info(info_prefix, "Terminating after wait for mutex.");
            break;
        }

        // Critical section: Retrieve message from queue
        memcpy(&msg, &queue->messages[queue->head_idx], sizeof(message_t));
        queue->head_idx = (queue->head_idx + 1) % QUEUE_CAPACITY;
        queue->free_slots++;
        queue->extracted_count++;
        current_extracted = queue->extracted_count;

        // Release mutex
        if (semaphore_op(SEM_MUTEX_IDX, 1) == -1) {
            print_error(info_prefix, "CRITICAL: Failed to release mutex!");
            goto cleanup_and_exit;
        }

        // Signal that an empty slot is available
        if (semaphore_op(SEM_EMPTY_IDX, 1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during signal empty (EINTR).");
                break;
            } else if (errno != EINTR) {
                print_error(info_prefix, "Semaphore signal (Empty) failed.");
                break;
            }
        }

        // Process the message (verify hash)
        original_hash = msg.hash;
        msg.hash = 0; // Zero out hash field for recalculation
        calculated_hash = calculate_message_hash(&msg);
        bool hash_ok = (original_hash == calculated_hash);

        printf("[%s] Extracted msg (Type:%u Size:%u Hash:%u -> %s). Total Extracted: %lu\r\n",
               info_prefix, msg.type, msg.size, original_hash,
               hash_ok ? "OK" : "FAIL", current_extracted);
        fflush(stdout);
        if (!hash_ok) {
            fprintf(stderr, "WARNING: [%s] Hash mismatch! Expected %u, Calculated %u\r\n",
                    info_prefix, original_hash, calculated_hash);
            fflush(stderr);
        }

        // Simulate work with a random delay
        struct timespec delay_req, delay_rem;
        long delay_us = (rand() % 400000L) + 200000L; // 0.2 to 0.6 seconds
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

    cleanup_and_exit: // Label fixed from cleanup_and_exit to avoid conflict if macro defined
    print_info(info_prefix, "Terminating gracefully...");
    fflush(stdout); // Attempt to flush stdio buffers
    fflush(stderr);
    _exit(EXIT_SUCCESS); // Use _exit for child processes
}
