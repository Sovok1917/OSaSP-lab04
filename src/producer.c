#include "producer.h"
#include "ipc_manager.h"
#include "utils.h"


static volatile sig_atomic_t s_child_terminate_flag = 0;

/*
 * child_signal_handler
 * Sets the child-local termination flag.
 */
static void child_signal_handler(int sig) {
    if (sig == SIGTERM) {
        s_child_terminate_flag = 1;
        const char msg[] = "[Producer] SIGTERM received\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
}

/*
 * producer_run (Implementation)
 */
void producer_run(int producer_id, queue_t *queue) {

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = child_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        print_error("Producer", "Failed to register SIGTERM handler");
        _exit(EXIT_FAILURE);
    }


    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    char info_prefix[32];
    snprintf(info_prefix, sizeof(info_prefix), "Producer %d", producer_id);
    print_info(info_prefix, "Started.");

    while (!s_child_terminate_flag) {
        message_t msg;


        msg.type = (unsigned char)(rand() % 256);
        msg.size = (unsigned char)(rand() % MAX_DATA_SIZE);
        for (int i = 0; i < msg.size; ++i) {
            msg.data[i] = (unsigned char)(rand() % 256);
        }
        msg.hash = 0;
        msg.hash = calculate_message_hash(&msg);



        while (semaphore_op(SEM_EMPTY_IDX, -1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during wait for empty slot (EINTR).");
                goto cleanup_and_exit;
            } else if (errno == EINTR) {
                continue;
            } else {

                print_error(info_prefix, "Semaphore wait (Empty) failed.");
                goto cleanup_and_exit;
            }
        }

        if (s_child_terminate_flag) {
            semaphore_op(SEM_EMPTY_IDX, 1);
            print_info(info_prefix, "Terminating after wait for empty slot.");
            break;
        }




        while (semaphore_op(SEM_MUTEX_IDX, -1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                semaphore_op(SEM_EMPTY_IDX, 1);
                print_info(info_prefix, "Terminating during wait for mutex (EINTR).");
                goto cleanup_and_exit;
            } else if (errno == EINTR) {
                continue;
            } else {
                semaphore_op(SEM_EMPTY_IDX, 1);
                print_error(info_prefix, "Semaphore wait (Mutex) failed.");
                goto cleanup_and_exit;
            }
        }

        if (s_child_terminate_flag) {
            semaphore_op(SEM_MUTEX_IDX, 1);
            semaphore_op(SEM_EMPTY_IDX, 1);
            print_info(info_prefix, "Terminating after wait for mutex.");
            break;
        }



        memcpy(&queue->messages[queue->tail_idx], &msg, sizeof(message_t));
        queue->tail_idx = (queue->tail_idx + 1) % QUEUE_CAPACITY;
        queue->free_slots--;
        queue->added_count++;
        unsigned long current_added = queue->added_count;



        if (semaphore_op(SEM_MUTEX_IDX, 1) == -1) {

            print_error(info_prefix, "CRITICAL: Failed to release mutex!");
            goto cleanup_and_exit;
        }



        if (semaphore_op(SEM_FULL_IDX, 1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during signal full (EINTR).");
                break;
            } else if (errno != EINTR) {
                print_error(info_prefix, "Semaphore signal (Full) failed.");

                break;
            }

        }


        printf("[%s] Added msg (Type:%u Size:%u Hash:%u). Total Added: %lu\r\n",
               info_prefix, msg.type, msg.size, msg.hash, current_added);
        fflush(stdout);


        struct timespec delay_req, delay_rem;
        long delay_us = (rand() % 400000L) + 100000L;
        delay_req.tv_sec = delay_us / 1000000L;
        delay_req.tv_nsec = (delay_us % 1000000L) * 1000L;
        while (nanosleep(&delay_req, &delay_rem) == -1) {
            if (errno == EINTR) {
                if (s_child_terminate_flag) break;
                delay_req = delay_rem;
            } else {
                print_error(info_prefix, "nanosleep failed"); break;
            }
        }


    }

    cleanup_and_exit:
    print_info(info_prefix, "Terminating gracefully...");
    _exit(EXIT_SUCCESS);
}
