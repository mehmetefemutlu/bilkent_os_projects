# Project 3: Resource Management Library

## Contributors

- Mehmet Efe Mutlu - 22303326
- Ahmet Eren Gokalp - 22302136
- Emir Said Bakan - 22302852

## Overview

This project implements a resource management library for CS342 Operating Systems Project 3. The library manages multiple resource types across multiple processes using POSIX shared memory and semaphores. It supports both deadlock detection and deadlock avoidance, and includes a demo application that shows the difference between the two modes.

## Files

- `rsm.h`: public API and library limits
- `rsm.c`: library implementation
- `myapp.c`: demo application that exercises the library
- `Makefile`: build rules for the static library and demo program

## Features

- Shared resource state stored in a single POSIX shared-memory segment
- Cross-process synchronization with POSIX semaphores
- Deadlock avoidance using Banker's algorithm
- Deadlock detection using resource-allocation-graph reduction
- Blocking requests that wake when resources become available and safe to grant

Modes:

- `0`: deadlock avoidance disabled. The demo intentionally creates a deadlock between two processes, and `rsm_detection()` reports it.
- `1`: deadlock avoidance enabled. Banker's algorithm prevents the unsafe allocation, so all processes complete.

## Demo Scenario

Setup:

- `3` processes: `P0`, `P1`, `P2`
- `5` resource types: `R0` through `R4`
- Each resource type has exactly one instance

Scenario summary:

- `P0` requests `R0`, waits, then requests `R1`
- `P1` waits, requests `R1`, waits, then requests `R0`
- `P2` requests `R2` and `R3`, then releases them

With mode `0`, `P0` and `P1` end up in a circular wait. With mode `1`, the unsafe request is delayed until the state becomes safe.

## Public API

The library exposes the following functions in `rsm.h`:

- `int rsm_init(int p_count, int r_count, int exist[], int avoid);`
  Initializes shared state for `p_count` processes and `r_count` resource types. `exist[]` contains the total number of instances per resource type. Set `avoid` to `1` to enable deadlock avoidance, or `0` for detection-only mode.

- `int rsm_destroy(void);`
  Destroys semaphores and unlinks the shared-memory segment.

- `int rsm_process_started(int apid);`
  Registers a process with application process id `apid`.

- `int rsm_process_ended(void);`
  Marks the calling process as finished and releases any resources it still holds.

- `int rsm_claim(int claim[]);`
  Declares a process's maximum claim. This is used only when avoidance mode is enabled.

- `int rsm_request(int request[]);`
  Requests resources. The call blocks if resources are currently unavailable or unsafe to grant.

- `int rsm_release(int release[]);`
  Releases resources currently held by the calling process.

- `int rsm_detection(void);`
  Detects deadlocked processes and returns their count.

- `void rsm_print_state(char headermsg[]);`
  Prints the current internal state for debugging.

## Implementation Notes

- All shared state is stored in one shared-memory object named `/cs342_rsm_shm`.
- A global semaphore protects library state.
- Each process has its own semaphore used to block and wake pending requests.
- A barrier semaphore delays requests until all processes have registered. In avoidance mode, the barrier opens after all claims are submitted.
- In avoidance mode, the library checks each request with Banker's safety test before granting it.
- The project links against `pthread` and `rt` as defined in the Makefile.
- If initialization fails because shared memory from a previous run still exists, remove the stale object and try again.
