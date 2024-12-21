# MIMPI - My Implementation Of MPI

This repository contains my implementation of the second lab assignment for the Concurrent Programming course at MIM UW. The project involved creating a simplified version of the Message Passing Interface (MPI) protocol in C.

Full assignment description can be found in [assignment.md](assignment.md). My code is in files `mimpirun.c`, `mimpi.c`, `mimpi_common.c`, `mimpi_common.h`.

## Overview

The goal of this project was to implement key functionalities of MPI for interprocess communication. It consists of:

- **`mimpirun`**: Handles the execution of parallel processes, managing their lifecycle and synchronization.
- **`mimpi` library**: Provides functions for communication and coordination between processes.

## Features

### `mimpirun` Program

The `mimpirun` program:
- Accepts the number of processes (`n`), the executable path (`prog`), and optional arguments.
- Launches `n` parallel instances of the specified program.
- Waits for all processes to finish and terminates cleanly.

### `mimpi` Library

The `mimpi` library includes:
1. **Initialization and Finalization**:
    - `MIMPI_Init`: Initializes resources.
    - `MIMPI_Finalize()`: Releases resources and cleans up.
2. **Communication**:
    - **Point-to-Point**: 
        - `MIMPI_Send`: Sends a message to a given process.
        - `MIMPI_Recv`: Waits for a message from a given process.
    - **Group**:
        - `MIMPI_Barrier`: Synchronizes all processes.
        - `MIMPI_Bcast`: Broadcasts data from one process to others.
        - `MIMPI_Reduce`: Aggregates data using operations like `sum`, `prod`, `max`, `min`.
3. **Process Information**:
    - `MIMPI_World_size()`: Returns the total number of processes.
    - `MIMPI_World_rank()`: Returns the calling process's rank.

## How to run

Build `mimpirun` and all examples in the `examples/` directory:
```bash
make
```

Example `mimpirun` usage:
```bash
./mimpirun <number of processes> <path to the executable> <optional arguments>
```

Run all tests:
```bash
./test
```

Run all tests with Valgrind:
```bash
VALGRIND=1 ./test
```

