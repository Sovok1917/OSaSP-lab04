#ifndef PRODUCER_H
#define PRODUCER_H

#include "common.h"



/*
 * producer_run
 *
 * Main function for a producer process.
 * Generates messages, adds them to the queue, and prints status.
 * Exits gracefully when s_child_terminate_flag is set.
 *
 * Accepts:
 *   producer_id - An identifier for this producer instance.
 *   queue       - Pointer to the shared memory queue.
 *
 * Returns:
 *   Does not return; calls exit().
 */

void producer_run(int producer_id, queue_t *queue);

#endif
