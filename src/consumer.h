#ifndef CONSUMER_H
#define CONSUMER_H

#include "common.h"



/*
 * consumer_run
 *
 * Main function for a consumer process.
 * Retrieves messages from the queue, verifies hash, and prints status.
 * Exits gracefully when s_child_terminate_flag is set.
 *
 * Accepts:
 *   consumer_id - An identifier for this consumer instance.
 *   queue       - Pointer to the shared memory queue.
 *
 * Returns:
 *   Does not return; calls exit().
 */

void consumer_run(int consumer_id, queue_t *queue);

#endif
