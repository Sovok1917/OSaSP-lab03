Parent-Child Process Interaction Demo
=====================================

This program demonstrates several concepts in POSIX systems programming, including:
- Process creation (fork, execv)
- Inter-process communication using signals (SIGUSR1, SIGUSR2, SIGKILL)
- Signal handling (sigaction, SIGALRM, SIGCHLD, SIGINT, etc.)
- Interval timers (setitimer)
- Terminal raw mode for single-character input (termios)
- Dynamic memory management for tracking child PIDs (malloc, realloc, free)
- Non-atomic updates to shared data and race condition demonstration (in the child).

Program Components:
-------------------
1.  parent: The main control program. It spawns and manages child processes.
2.  child: A worker program. It repeatedly updates a shared data structure non-atomically.
    A SIGALRM timer interrupts these updates, and the child records the state of the
    data structure at the time of interruption to observe potential race conditions
    (intermediate states like {0,1} or {1,0} when only {0,0} or {1,1} are intended).

Build Instructions:
-------------------
The project uses a Makefile for building. Source code is expected in the src/ directory,
and build artifacts will be placed in the build/ directory (in build/debug or
build/release subdirectories).

1.  Build Debug Version (Default):
    make
    or
    make debug-build
    Executables: build/debug/parent, build/debug/child

2.  Build Release Version:
    (Treats warnings as errors and applies optimizations)
    make release-build
    Executables: build/release/parent, build/release/child

3.  Clean Build Artifacts:
    make clean
    This removes the entire build/ directory.

4.  Show Help:
    make help
    This displays available make targets and their descriptions.

Running the Program:
--------------------
The parent program needs to know where to find the child executable. This is
done by setting the CHILD_PATH environment variable to the directory containing
the child executable. The Makefile provides convenient run targets that handle this.

1.  Run Debug Version:
    make run
    This will build the debug version (if necessary) and then execute build/debug/parent,
    automatically setting CHILD_PATH to build/debug/.

2.  Run Release Version:
    make run-release
    This will build the release version (if necessary) and then execute build/release/parent,
    automatically setting CHILD_PATH to build/release/.

Manual Execution (Example):
If you build manually and want to run:
# Assuming you built the debug version
export CHILD_PATH="$(pwd)/build/debug"
./build/debug/parent

Parent Program Commands (Input single characters):
-------------------------------------------------
Once the parent program is running, it will enter raw terminal mode and accept
the following single-character commands:

*   + : Spawn a new child process.
*   - : Kill the most recently spawned child process (sends SIGKILL).
*   l : List the PIDs of the parent and all currently tracked child processes.
*   k : Kill all currently tracked child processes (sends SIGKILL).
*   1 : Send SIGUSR1 to all children, instructing them to ENABLE their statistics output
        (if they were previously disabled).
*   2 : Send SIGUSR2 to all children, instructing them to DISABLE their statistics output.
*   q : Quit the parent program. This will also attempt to kill all remaining children.

Child Program Behavior:
-----------------------
-   Upon startup, the child process begins rapidly alternating a shared pair of integers
    between {0,0} and {1,1}.
-   A SIGALRM timer is set to interrupt these updates at short intervals (e.g., 500 microseconds).
-   The SIGALRM handler reads the state of the shared pair. Due to the non-atomic nature
    of the update (two separate assignments), the handler might observe intermediate states
    like {0,1} or {1,0} in addition to the intended {0,0} and {1,1}.
-   It counts occurrences of each observed state: {0,0}, {0,1}, {1,0}, {1,1}.
-   After a predefined number of timer repetitions (NUM_REPETITIONS), the child process
    will print these statistics to its standard output, prefixed with its PID and its parent's PID.
    Example: PPID=123, PID=124, STATS={00:2500, 01:50, 10:45, 11:2405}
-   The child's statistics output can be enabled (default) or disabled by the parent sending
    SIGUSR1 or SIGUSR2 respectively. If disabled, the child prints a message to its
    stderr indicating that output was suppressed.
-   The child process prints diagnostic messages to its standard error (stderr).

Notes:
------
-   The parent process uses stderr for its own diagnostic messages and status updates
    to keep stdout clean for potential child output redirection or cleaner display.
-   The child process uses stdout for its final statistics report and stderr for its
    own diagnostic messages.
-   The use of \r\n in some stderr messages from the parent is to ensure proper
    line breaks when the terminal is in raw mode.
-   The program demonstrates graceful shutdown via signal handling (SIGINT, SIGTERM, SIGQUIT)
    and an atexit handler in the parent.
