# MIMPI

[MPI](https://pl.wikipedia.org/wiki/Message_Passing_Interface)
is a standard communication protocol for exchanging data
between parallel program processes, used primarily in supercomputing. 

The purpose of the task, as the name _MIMPI_ - which is an acronym for _My Implementation of MPI_ - suggests, is to implement a small, slightly modified piece of MPI. You are required to implement the following components according to the specifications below:

- the `mimpirun` program code (in `mimpirun.c`) that runs the parallel computations,
- the `mimpi.c` implementation of the procedures declared in `mimpi.h`.

## The `mimpirun` program

The `mimpirun` program takes the following command-line arguments:

1) $n$ - the number of instances to run (a natural number between 1 and 16, inclusive).
2) $prog$ - the path to an executable file (it may reside in PATH). If the `exec` call fails (e.g., due to an invalid path), `mimpirun` should exit with a non-zero exit code.
3) $args$ - optional and unlimited arguments to be passed to all instances of the $prog$ program.

The `mimpirun` program performs the following sequentially (each action starts only after the previous one is fully completed):

1) Prepares the environment (the specific steps are up to the implementer).
2) Launches $n$ instances of the $prog$ program, each in a separate process.
3) Waits for all created processes to terminate.
4) Ends execution.

## Assumptions about $prog$ programs

- $prog$ programs may enter the _MPI block_ **only once** during their execution. This is done by calling the library function `MIMPI_Init` at the start and `MIMPI_Finalize` at the end. The _MPI block_ includes all code executed between these calls.
- While in the _MPI block_, programs may use various procedures from the `mimpi` library to communicate with other $prog$ processes.
- Programs may perform operations (read, write, open, close, etc.) on files whose file descriptors fall within the ranges $[0,19]$ and $[1024, \infty)$ (including `STDIN`, `STDOUT`, and `STDERR`).
- Programs do not modify environment variables with names beginning with the `MIMPI` prefix.
- Programs assume properly set arguments, meaning the zeroth argument, by Unix convention, is the $prog$ program name, and subsequent arguments correspond to $args$.

## The `mimpi` Library

You must implement the following procedures with the signatures provided in the `mimpi.h` header file:

### Helper Procedures

- `void MIMPI_Init(bool enable_deadlock_detection)`

    Starts the MPI block, initializing resources needed for the `mimpi` library. The `enable_deadlock_detection` flag enables deadlock detection for the duration of the _MPI block_.

- `void MIMPI_Finalize()`

    Ends the _MPI block_. 
    All resources used by the `mimpi` library, such as:
    - Open files
    - Open communication channels
    - Allocated memory
    - Synchronization primitives
    - etc
    
    should be freed before this procedure ends.

- `int MIMPI_World_size()`

    Returns the number of $prog$ processes launched by `mimpirun` (equal to the $n$ parameter passed to `mimpirun`).

- `int MIMPI_World_rank()`:

    Returns a unique identifier for the process within the group of processes launched by `mimpirun`. Identifiers should be sequential natural numbers from $0$ to $n-1$.

### Point-to-Point communication procedures

- `MIMPI_Retcode MIMPI_Send(void const *data, int count, int destination, int tag)`

    Sends data from the address `data` as a byte array of size `count` to the process with rank `destination`, attaching a `tag` to the message.

    If `MIMPI_Send` is called for a process that has already exited the _MPI block_, it should fail immediately with the error code `MIMPI_ERROR_REMOTE_FINISHED`.

- `MIMPI_Retcode MIMPI_Recv(void *data, int count, int source, int tag)`

    Waits for a message of size (exactly) `count` with the `tag` from the process with rank `source` and stores its contents at the address `data`. The caller is responsible for ensuring sufficient allocated memory.
    The call is blocking, meaning it completes only after the entire message is received. 
    
    If the sender exits the _MPI block_ without sending a matching message, this function should return `MIMPI_ERROR_REMOTE_FINISHED`.

    - Basic Version:
    You can assume that each process sends messages exactly in the order that the recipient expects to receive them. However, you **cannot assume** that multiple processes **will not send** messages to the same recipient simultaneously. Additionally, you may assume that the data associated with a single message does not exceed 512 bytes.

    - **Improvement 1**:
    Messages can be arbitrarily (but reasonably) large, potentially exceeding the size of the connection buffer (`pipe`).

    - **Improvement 2**:
    You cannot assume any order of the messages being sent. The recipient should buffer incoming messages and, when `MIMPI_Recv` is called, return the first message (by arrival time) that matches the `count`, `source`, and `tag` parameters. (You do not need to implement a complex mechanism for selecting the next matching packet; linear complexity concerning the number of unprocessed packets for the target process is sufficient.)

    - **Improvement 3**:
    The recipient should process incoming messages concurrently with performing other actions to avoid filling the message-sending channels. In other words, sending many messages should not block the sender, even if the recipient is not processing them, as they should go into a constantly growing buffer.

### Group communication procedures

#### General requirements

Each group communication procedure $p$ serves as a **synchronization point** for all processes. This means that instructions following the $i$-th invocation of $p$ in any process are executed **after** all instructions preceding the $i$-th invocation of $p$ in any other process.

If the synchronization of all processes cannot be completed because one of the processes has already exited the _MPI block_, the invocation of `MIMPI_Barrier` in at least one process should return the error code `MIMPI_ERROR_REMOTE_FINISHED`. If the process encountering this error terminates as a result, the invocation of `MIMPI_Barrier` should fail in at least one other process. Repeating this behavior, the system should eventually reach a state where every process exits the barrier with an error.

#### Efficiency

Each group communication procedure $p$ should be implemented efficiently. Specifically, assuming that deadlock detection is disabled, the time between the invocation of $p$ by the last process and the completion of $p$ by the last process should not exceed:
$\lceil w / 256 \rceil(3\left \lceil\log_2(n+1)-1 \right \rceil t+\epsilon)$

where:

- $n$: the number of processes,
- $t$: the maximum execution time of `chsend` for transmitting a single message during the given invocation of the group communication function
- $\epsilon$: a small constant (of the order of milliseconds at most) that does not depend on $t$,
- $w$: the size of the message (in bytes) processed in the given invocation of the group communication function (for `MIMPI_Barrier`, assume $w=1$).

Additionally, for the implementation to be considered efficient, the transmitted data should not be accompanied with excessive metadata. In particular, we expect that group functions called for data sizes
less than 256 bytes will call `chsend` and `chrecv` for packets of
size less than or equal to 512 bytes.

The `tests/effectiveness` directory includes tests to verify the above-defined notion of efficiency.

#### Available Procedures

- `MIMPI_Retcode MIMPI_Barrier()`

    Synchronizes all processes.

- `MIMPI_Retcode MIMPI_Bcast(void *data, int count, int root)`
    Sends data provided by the process with rank `root` to all other processes.

- `MIMPI_Retcode MIMPI_Reduce(const void *send_data, void *recv_data, int count, MPI_Op op, int root)`

    Collects data provided by all processes in `send_data` (treating it as an array of `uint8_t` values of size `count`) and performs a reduction operation of type `op` on elements at the same indices from the `send_data` arrays of all processes (including `root`).
    The result of the reduction — an array of `uint8_t` values of size `count` — is stored at the address `recv_data` **only** in the process with rank `root` (writing to `recv_data` in other processes is **not allowed**).

    The following reduction operations (values of the `MIMPI_Op enum`) are available:
    - `MIMPI_MAX`: maximum,
    - `MIMPI_MIN`: minimum,
    - `MIMPI_SUM`: sum,
    - `MIMPI_PROD`: product.

    It is essential to note that all the above operations on the available data types are commutative and associative, and `MIMPI_Reduce` should be optimized accordingly.

### `MIMPI_Retcode` semantics

Refer to the documentation in the `mimpi.h` code for:
    - the `MIMPI_Retcode` documentation,
    - documentation of individual procedures returning `MIMPI_Retcode`.

### Tag semantics

The following conventions are adopted:

- `tag > 0`: reserved for library users for their own needs,
- `tag = 0`: represents `ANY_TAG`. Its use in `MIMPI_Recv` allows matching messages with any tag. It should not be used in `MIMPI_Send` (the behavior is undefined),
- `tag < 0`: reserved for library implementers and may be used for internal communication.

In particular, this means that user programs (e.g., our test programs) will never directly invoke `MIMPI_Send` or `MIMPI_Recv` with a tag `< 0`.

## Interprocess Communication

The MPI standard is designed for computations run on supercomputers. Consequently, communication between individual processes typically occurs over a network and is slower than data exchange within a single computer.

To better simulate the environment of a real library and address its implementation challenges, communication between processes must be conducted **exclusively** using channels provided in the `channel.h` library. The `channel.h` library provides the following functions for channel management:

- `void channels_init()`: Initializes the channel library.
- `void channels_finalize()`: Finalizes the channel library.
- `int channel(int pipefd[2])`: Creates a channel.
- `int chsend(int __fd, const void *__buf, size_t __n)`: Sends a message.
- `int chrecv(int __fd, void *__buf, size_t __nbytes)`: Receives a message.

The `channel`, `chsend`, and `chrecv` functions behave similarly to `pipe`, `write`, and `read`, respectively. However, the key difference for this task is that the functions provided by `channel.h` may take significantly longer to execute than their standard counterparts. Specifically, the provided functions:

- Have the same signatures as their standard counterparts,
- Similarly create entries in the open file table,
- Guarantee atomic reads and writes for up to 512 bytes,
- Guarantee a buffer size of at least 4 KB,

**NOTE:**
The following helper functions must be called: `channels_init` in `MIMPI_Init`, and `channels_finalize` in `MIMPI_Finalize`.

**All** reads and writes for file descriptors returned by the `channel` function must be performed using `chsend` and `chrecv`. Additionally, system functions modifying file properties (e.g., `fcntl`) should not be called for file descriptors returned by the channel function.

Keep in mind that the guarantees for `chsend` and `chrecv` do not imply that they will process the exact number of bytes requested. This may occur if the size exceeds the guaranteed buffer size of the channel or if the input buffer does not contain enough data. Your implementation must handle such situations correctly.

## Notes

### General

- The `mimpirun` program or any of the functions from the `mimpi` library **cannot** create named files in the file system.
- The `mimpirun` program and functions from the `mimpi` library may use file descriptors numbered in the range $[20, 1023]$ in any way. Additionally, you can assume that descriptors in this range are unoccupied when the `mimpirun` program is launched.
- The `mimpirun` program or any of the functions from the `mimpi` library **cannot** modify existing entries in the open file table outside the range $[20, 1023]$.
- The `mimpirun` program or any of the functions from the `mimpi` library **cannot** perform operations on files they did not open themselves (particularly not on `STDIN`, `STDOUT`, or `STDERR`).
- Active or semi-active waiting must not be used at any point.
    - Specifically, you must not use any function that pauses program execution for a specific time (e.g., `sleep`, `usleep`, `nanosleep`) or any variants of functions with timeouts (e.g., `select`).
    - You must wait **only** for time-independent events, such as the arrival of a message.
- Solutions will be tested for memory leaks and/or other resource leaks (e.g., unclosed files). You should carefully trace and test paths that might lead to leaks.
- You can assume that corresponding $i$-th invocations of group communication functions in different processes are of the same type (i.e., the same function) and have identical parameter values for `count`, `root`, and `op` (if the current function type has such parameters).
- In the event of a system function error, the calling program must terminate with a non-zero exit code, e.g., by using the provided `ASSERT_SYS_OK` macro.
- If $prog$ programs use the library in a way that violates the guarantees listed in this document, you may handle the situation in any manner (such cases will not be tested).

### `MIMPI` library

- The implemented functions do not need to be _thread-safe_, i.e., you can assume they are not called simultaneously from multiple threads.
- The function implementations should be reasonably efficient. Specifically, under normal conditions (e.g., not handling hundreds of thousands of messages), they should not add delays of tens of milliseconds (or more) to the expected runtime (e.g., waiting for a message).
- Invoking any procedure other than `MIMPI_Init` outside of an MPI block has undefined behavior.
- Multiple invocations of the `MIMPI_Init` procedure have undefined behavior.
- We guarantee that `channels_init` will set the `SIGPIPE` signal to be ignored. This simplifies meeting the requirement that `MIMPI_Send` must return `MIMPI_ERROR_REMOTE_FINISHED` in the appropriate scenario.

## Allowed languages and libraries

We require the use of the C programming language in the `gnu11` standard (as `c11` alone does not provide access to many useful functions in the standard library).
This requirement is strict because one goal of this task is to deepen your proficiency with C.

You may use the standard C library (`libc`), the `pthread` library, and system-provided functionality (declared in `unistd.h`, etc.).

Other external libraries are **not allowed**.

You may borrow any code from laboratory exercises. Any other borrowed code must be appropriately commented, with the source cited.

## Package description

The package includes the following files, which are not part of the solution:

- `examples/*`: Simple example programs using the `mimpi` library.
- `tests/*`: Tests for various configurations of example programs run using `mimpirun`.
- `assignment.md`: This description file.
- `channel.h`: A header file declaring functions for interprocess communication.
- `channel.c`: An example implementation of `channel.h`.
- `mimpi.h`: A header file declaring functions of the `MIMPI` library.
- `Makefile`: A sample file for automating the compilation of mimpirun, example programs, and test execution.
- `test`: A script to locally run all tests in the `tests/` directory.
- `files_allowed_for_change`: A script listing the files that may be modified.

Templates to complete:

- `mimpi.c`: Contains skeletons for implementing the functions of the `MIMPI` library.
- `mimpirun.c`: Contains the skeleton for implementing the `mimpirun` program.
- `mimpi_common.h`: A header file for declaring shared functionality between the `MIMPI` library and the `mimpirun` program.
- `mimpi_common.c`: A file for implementing shared functionality between the `MIMPI` library and the `mimpirun` program.

### Useful Commands

- Build `mimpirun` and all examples in the `examples/` directory: `make`
- Run local tests: `./test`
- Run local tests with Valgrind: `VALGRIND=1 ./test`
- List all files opened by processes launched by mimpirun: `./mimpirun 2 ls -l /proc/self/fd`
- Debug memory and resource leaks:
    Tools like `Valgrind` can be useful, especially with these flags:
    - `--track-origins=yes`
    - `--track-fds=yes`

    Another helpful tool with a narrower scope is _ASAN_ (Address Sanitizer).
        It can be enabled by passing the `-fsanitize=address` flag to `gcc`.