Producer-Consumer Demo with System V IPC
========================================

This program demonstrates a classic producer-consumer problem solved using
System V Inter-Process Communication (IPC) mechanisms:
- Shared Memory (shmget, shmat, shmdt, shmctl) for the shared queue.
- Semaphores (semget, semop, semctl) for synchronization (mutex, empty slots, full slots).

The main process allows dynamic creation and termination of producer and consumer
child processes via keyboard commands.

Program Components:
-------------------
1.  main (prod_cons_ipc): The main control program. It initializes IPC resources,
    sets up the terminal for non-blocking input, and then allows the user to:
    - Create new producer processes ('p').
    - Create new consumer processes ('c').
    - Stop the last created producer ('P').
    - Stop the last created consumer ('C').
    - Display the current status of the queue and processes ('s').
    - Quit the application ('q'), which cleans up IPC resources and terminates children.

2.  producer: Child process that generates messages (with a type, size, data, and hash)
    and adds them to the shared queue. It uses semaphores to wait for an empty slot
    and to protect access to the queue.

3.  consumer: Child process that retrieves messages from the shared queue. It uses
    semaphores to wait for a full slot and to protect access to the queue. It then
    verifies the hash of the received message.

4.  ipc_manager: Module responsible for initializing, managing, and cleaning up
    System V IPC resources (shared memory segment and semaphore set). It also
    handles the creation and termination signaling of producer/consumer processes.

5.  utils: Utility functions for terminal manipulation (non-blocking, no-echo input),
    message hash calculation, and printing formatted info/error messages.

Build Instructions:
-------------------
The project uses a Makefile for building. Source code is expected in the src/ directory,
and build artifacts will be placed in the build/ directory (in build/debug or
build/release subdirectories).

1.  Build Debug Version (Default):
    make
    or
    make debug-build
    Executable: build/debug/prod_cons_ipc

2.  Build Release Version:
    (Treats warnings as errors and applies optimizations)
    make release-build
    Executable: build/release/prod_cons_ipc

3.  Clean Build Artifacts:
    make clean
    This removes the entire build/ directory.

4.  Show Help:
    make help
    This displays available make targets and their descriptions.

Running the Program:
--------------------
1.  Run Debug Version:
    make run
    This will build the debug version (if necessary) and then execute build/debug/prod_cons_ipc.

2.  Run Release Version:
    make run-release
    This will build the release version (if necessary) and then execute build/release/prod_cons_ipc.

Manual Execution (Example):
# Assuming you built the debug version
./build/debug/prod_cons_ipc

Program Commands (Input single characters):
-------------------------------------------
Once the program is running, it will display a menu and accept the following
single-character commands:

*   p : Add a new Producer process.
*   c : Add a new Consumer process.
*   P : Stop (SIGTERM) the last added Producer process.
*   C : Stop (SIGTERM) the last added Consumer process.
*   s : Show current status (queue details, number of active producers/consumers).
*   q : Quit the application. This will send SIGTERM to all child processes and
        attempt to clean up all System V IPC resources.

Child Process Behavior:
-----------------------
-   Producers generate messages with random data, calculate a hash, and attempt to
    add them to the shared queue. They print a status message upon adding.
-   Consumers attempt to retrieve messages from the queue, recalculate the hash of the
    message data (after temporarily zeroing the received hash field), and compare it
    with the original hash. They print a status message including hash verification (OK/FAIL).
-   Both producers and consumers introduce random delays to simulate work.
-   Child processes (producers and consumers) are designed to terminate gracefully
    when they receive a SIGTERM signal from the parent.

IPC Resource Cleanup:
---------------------
-   The program attempts to clean up System V shared memory segments and semaphore sets
    upon normal termination ('q' command) or via an atexit handler if the parent
    exits unexpectedly (e.g., due to some signals not caught by the custom handler).
-   If IPC resources are not cleaned up properly (e.g., due to a crash without the
    atexit handler running or SIGKILL), they might persist in the system.
    You can list System V IPC resources using:
      ipcs -m  (for shared memory)
      ipcs -s  (for semaphores)
    And remove them using:
      ipcrm -m <shmid>
      ipcrm -s <semid>

Notes:
------
-   The program uses non-blocking, no-echo terminal input for interactive commands.
-   Error messages are typically printed to stderr, while informational messages and
    producer/consumer outputs are printed to stdout.
-   The program demonstrates signal handling in both parent and child processes for
    graceful shutdown.
