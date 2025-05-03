/*
 * parent.c
 *
 * Parent process for managing child processes based on keyboard input.
 * Spawns children ('+'), deletes the last one ('-'), lists all ('l'),
 * kills all ('k'), enables child output ('1'), disables child output ('2'),
 * or quits ('q').
 * Children execute the 'child' program found via the CHILD_PATH env variable.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdint.h>


#define CHILD_PROG_NAME "child"
#define INITIAL_CHILD_CAPACITY 8
#define MAX_PATH_LEN 1024


static pid_t *g_child_pids = NULL;
static size_t g_child_count = 0;
static size_t g_child_capacity = 0;
static struct termios g_orig_termios;
static char g_child_exec_path[MAX_PATH_LEN];
static volatile sig_atomic_t g_terminate_flag = 0;


typedef struct termios termios_t;


static void enable_raw_mode(void);
static void disable_raw_mode(void);
static void cleanup_resources(void);
static void handle_signal(int sig);
static void register_signal_handlers(void);
static int add_child_pid(pid_t pid);
static void remove_child_pid_at_index(size_t index);
static void kill_all_children(const char *reason);
static void signal_all_children(int sig);
static void spawn_child(void);
static void kill_last_child(void);
static void list_children(void);
static void initialize_globals(void);
static ssize_t safe_write(int fd, const void *buf, size_t count);


/*
 * main
 *
 * Entry point of the parent process.
 * Sets up terminal, signal handlers, and the main command loop.
 *
 * Accepts:
 *   argc - Argument count (expected: 1)
 *   argv - Argument vector (unused)
 *
 * Returns:
 *   EXIT_SUCCESS on normal exit.
 *   EXIT_FAILURE on critical errors (e.g., setup failure).
 */
int main(int argc, char *argv[]) {
    (void)argv;


    initialize_globals();


    if (argc > 1) {
        if (fprintf(stderr, "Info: This program doesn't expect command-line arguments.\n") < 0) { /* Handle error? */ }
        if (fprintf(stderr, "      It uses the CHILD_PATH environment variable to find the child executable.\n") < 0) { /* Handle error? */ }
    }


    const char *child_path_dir = getenv("CHILD_PATH");
    if (child_path_dir == NULL) {
        if (fprintf(stderr, "Error: CHILD_PATH environment variable not set.\n") < 0) { /* Handle error? */ }
        if (fprintf(stderr, "       Please set CHILD_PATH to the directory containing the '%s' executable.\n", CHILD_PROG_NAME) < 0) { /* Handle error? */ }
        return EXIT_FAILURE;
    }

    int path_len = snprintf(g_child_exec_path, sizeof(g_child_exec_path), "%s/%s", child_path_dir, CHILD_PROG_NAME);
    if (path_len < 0 || (size_t)path_len >= sizeof(g_child_exec_path)) {
        if (fprintf(stderr, "Error: Child executable path is too long or formatting error.\n") < 0) { /* Handle error? */ }
        return EXIT_FAILURE;
    }

    if (access(g_child_exec_path, X_OK) != 0) {

        if (fprintf(stderr, "Error: Child executable '%s' not found or not executable (errno %d: %s).\n",
            g_child_exec_path, errno, strerror(errno)) < 0) { /* Handle error? */ }
            return EXIT_FAILURE;
    }



    if (atexit(cleanup_resources) != 0) {
        perror("Error: Failed to register atexit cleanup function");

    }
    enable_raw_mode();
    register_signal_handlers();

    g_child_pids = malloc(INITIAL_CHILD_CAPACITY * sizeof(pid_t));
    if (g_child_pids == NULL) {
        disable_raw_mode();
        perror("Error: Failed to allocate memory for child PIDs");

        exit(EXIT_FAILURE);
    }
    g_child_capacity = INITIAL_CHILD_CAPACITY;
    g_child_count = 0;


    if (printf("Parent process started (PID: %d).\r\n", getpid()) < 0) { /* Handle error? */ }
    if (printf("Commands: '+' spawn, '-' kill last, 'l' list, 'k' kill all,\r\n") < 0) { /* Handle error? */ }
    if (printf("          '1' enable child output (SIGUSR1), '2' disable child output (SIGUSR2),\r\n") < 0) { /* Handle error? */ }
    if (printf("          'q' quit.\r\n") < 0) { /* Handle error? */ }
    if (printf("Using child executable: %s\r\n", g_child_exec_path) < 0) { /* Handle error? */ }
    if (fflush(stdout) == EOF) {
        disable_raw_mode();
        perror("PARENT: Error flushing initial messages");
        exit(EXIT_FAILURE);
    }


    char c;
    while (!g_terminate_flag) {
        ssize_t read_result = read(STDIN_FILENO, &c, 1);

        if (read_result == 1) {
            switch (c) {
                case '+':
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    spawn_child();
                    break;
                case '-':
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    kill_last_child();
                    break;
                case 'l':
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    list_children();
                    break;
                case 'k':
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    kill_all_children("Received 'k' command.");

                    break;
                case '1':
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    signal_all_children(SIGUSR1);
                    break;
                case '2':
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    signal_all_children(SIGUSR2);
                    break;
                case 'q':
                    safe_write(STDOUT_FILENO, "\r\n", 2);

                    if (fprintf(stderr, "PARENT [%d]: Received 'q' command. Initiating shutdown.\r\n", getpid()) < 0) { /* Handle error? */ }
                    g_terminate_flag = 1;
                    break;
                default:

                    break;
            }
            if (fflush(stdout) == EOF) {
                fprintf(stderr, "Warning: fflush(stdout) failed in command loop.\n");
            }
            if (fflush(stderr) == EOF) {
                fprintf(stderr, "Warning: fflush(stderr) failed in command loop.\n");
            }
        } else if (read_result == 0) {

            safe_write(STDOUT_FILENO, "\r\n", 2);
            if (fprintf(stderr, "PARENT [%d]: EOF detected on stdin. Initiating shutdown.\r\n", getpid()) < 0) { /* Handle error? */ }
            g_terminate_flag = 1;
        } else if (errno == EINTR) {

            continue;
        } else {

            disable_raw_mode();
            perror("PARENT: Error reading from stdin");
            exit(EXIT_FAILURE);
        }
    }



    return EXIT_SUCCESS;
}

/*
 * initialize_globals
 *
 * Explicitly initializes static global variables at runtime.
 *
 * Accepts: None
 * Returns: None
 */
static void initialize_globals(void) {
    g_child_pids = NULL;
    g_child_count = 0;
    g_child_capacity = 0;
    memset(&g_orig_termios, 0, sizeof(g_orig_termios));
    g_child_exec_path[0] = '\0';
    g_terminate_flag = 0;
}

/*
 * safe_write
 *
 * Wrapper around write() to handle EINTR and partial writes.
 * Suitable for use outside signal handlers. For signal handlers,
 * direct write() with loop might be needed if partial writes are possible.
 *
 * Accepts:
 *   fd - File descriptor
 *   buf - Buffer to write
 *   count - Number of bytes to write
 *
 * Returns:
 *   Total bytes written, or -1 on error (sets errno).
 */
static ssize_t safe_write(int fd, const void *buf, size_t count) {
    size_t total_written = 0;
    const char *ptr = (const char *)buf;

    while (total_written < count) {
        ssize_t written = write(fd, ptr + total_written, count - total_written);
        if (written == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {

            errno = EIO;
            return -1;
        }
        total_written += (size_t)written;
    }
    return (ssize_t)total_written;
}


/*
 * enable_raw_mode
 *
 * Switches the terminal controlling stdin to raw mode.
 * Disables echo and canonical processing. Saves original settings.
 * Exits on failure.
 *
 * Accepts: None
 * Returns: None
 */
static void enable_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) {
        if (fprintf(stderr, "Error: Standard input is not a terminal. Raw mode not applicable.\n") < 0) { /* Handle error? */ }
        exit(EXIT_FAILURE);
    }
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) {
        perror("Error: tcgetattr failed");
        exit(EXIT_FAILURE);
    }

    termios_t raw = g_orig_termios;

    raw.c_iflag &= ~(unsigned long)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned long)(OPOST);
    raw.c_cflag |= (unsigned long)(CS8);
    raw.c_lflag &= ~(unsigned long)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("Error: tcsetattr failed to enable raw mode");

        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        exit(EXIT_FAILURE);
    }
}

/*
 * disable_raw_mode
 *
 * Restores the terminal settings for stdin to their original state.
 * Ensures cursor is on a new line. Uses safe_write for output.
 *
 * Accepts: None
 * Returns: None
 */
static void disable_raw_mode(void) {

    if (!isatty(STDIN_FILENO)) {
        return;
    }


    if (g_orig_termios.c_lflag != 0 || g_orig_termios.c_iflag != 0 || g_orig_termios.c_oflag != 0) {
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios) == -1) {

            const char msg[] = "Warning: Failed to restore terminal attributes.\n";
            safe_write(STDERR_FILENO, msg, sizeof(msg) - 1);

        }
    }

    safe_write(STDOUT_FILENO, "\r\n", 2);

}

/*
 * cleanup_resources
 *
 * Frees allocated memory, kills remaining children, and restores terminal mode.
 * Intended to be registered with atexit(). Uses stderr for messages via safe_write.
 *
 * Accepts: None
 * Returns: None
 */
static void cleanup_resources(void) {

    disable_raw_mode();


    pid_t pid = getpid();
    char msg_buf[128];
    int len = snprintf(msg_buf, sizeof(msg_buf), "PARENT [%d]: Cleaning up...\r\n", pid);
    if (len > 0) {
        safe_write(STDERR_FILENO, msg_buf, (size_t)len);
    }

    kill_all_children("Parent exiting.");

    if (g_child_pids != NULL) {
        free(g_child_pids);
        g_child_pids = NULL;
        g_child_count = 0;
        g_child_capacity = 0;
    }

    len = snprintf(msg_buf, sizeof(msg_buf), "PARENT [%d]: Cleanup complete.\r\n", pid);
    if (len > 0) {
        safe_write(STDERR_FILENO, msg_buf, (size_t)len);
    }

}

/*
 * handle_signal
 *
 * Signal handler for SIGINT, SIGTERM, SIGQUIT (sets termination flag)
 * and SIGCHLD (reaps zombies). Async-signal-safe.
 * IMPORTANT: This handler only reaps zombies; it does NOT update g_child_pids.
 *
 * Accepts:
 *   sig - The signal number received.
 *
 * Returns: None
 */
static void handle_signal(int sig) {
    int saved_errno = errno;

    if (sig == SIGCHLD) {
        pid_t pid;

        while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {






        }


        if (pid == -1 && errno != ECHILD) {
            const char msg[] = "PARENT: Error in waitpid (SIGCHLD handler).\n";
            safe_write(STDERR_FILENO, msg, sizeof(msg) - 1);
        }
    } else if (sig == SIGINT || sig == SIGTERM || sig == SIGQUIT) {


        g_terminate_flag = 1;

        const char msg[] = "\r\nPARENT: Termination signal received, initiating shutdown...\r\n";
        safe_write(STDERR_FILENO, msg, sizeof(msg) - 1);


    }

    errno = saved_errno;
}


/*
 * register_signal_handlers
 *
 * Sets up signal handlers for SIGINT, SIGTERM, SIGQUIT, and SIGCHLD.
 * Exits on failure.
 *
 * Accepts: None
 * Returns: None
 */
static void register_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    if (sigemptyset(&sa.sa_mask) == -1) {
        disable_raw_mode();
        perror("Error: sigemptyset failed");
        exit(EXIT_FAILURE);
    }




    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1 ||
        sigaction(SIGQUIT, &sa, NULL) == -1) {
        disable_raw_mode();
    perror("Error: Failed to register termination signal handlers");
    exit(EXIT_FAILURE);
        }



        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        if (sigaction(SIGCHLD, &sa, NULL) == -1) {
            disable_raw_mode();
            perror("Error: Failed to register SIGCHLD handler");
            exit(EXIT_FAILURE);
        }


        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN;
        sa.sa_flags = 0;
        if (sigaction(SIGUSR1, &sa, NULL) == -1 || sigaction(SIGUSR2, &sa, NULL) == -1) {

            fprintf(stderr, "Warning: Failed to ignore SIGUSR1/SIGUSR2 in parent.\n");
        }
}


/*
 * add_child_pid
 *
 * Adds a child PID to the dynamic array, resizing if necessary.
 * Aborts on memory allocation failure.
 *
 * Accepts:
 *   pid - The PID of the child process to add.
 *
 * Returns:
 *   0 on success. Aborts on failure.
 */
static int add_child_pid(pid_t pid) {
    if (g_child_count >= g_child_capacity) {
        size_t new_capacity = (g_child_capacity == 0) ? INITIAL_CHILD_CAPACITY : g_child_capacity * 2;

        if (new_capacity < g_child_capacity || (new_capacity > (SIZE_MAX / sizeof(pid_t)))) {
            disable_raw_mode();
            if (fprintf(stderr, "Error: Child PID array capacity overflow.\r\n") < 0) { /* Handle error? */ }
            abort();
        }
        pid_t *new_pids = realloc(g_child_pids, new_capacity * sizeof(pid_t));
        if (new_pids == NULL) {

            disable_raw_mode();
            perror("Error: Failed to reallocate memory for child PIDs");

            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            abort();
        }
        g_child_pids = new_pids;
        g_child_capacity = new_capacity;
    }
    g_child_pids[g_child_count++] = pid;
    return 0;
}

/*
 * remove_child_pid_at_index
 *
 * Removes a child PID from the array at the specified index by shifting
 * subsequent elements down. Assumes index is valid relative to current count.
 *
 * Accepts:
 *   index - The index of the PID to remove.
 *
 * Returns: None
 */
static void remove_child_pid_at_index(size_t index) {
    pid_t parent_pid = getpid();
    if (index >= g_child_count) {

        if (fprintf(stderr, "PARENT [%d]: Error: Invalid index %zu in remove_child_pid_at_index (count=%zu).\r\n",
            parent_pid, index, g_child_count) < 0) { /* Handle error? */ }
            return;
    }


    size_t elements_to_move = g_child_count - index - 1;
    if (elements_to_move > 0) {
        memmove(&g_child_pids[index], &g_child_pids[index + 1], elements_to_move * sizeof(pid_t));
    }

    g_child_count--;













}


/*
 * kill_all_children
 *
 * Sends SIGKILL to all currently tracked child processes.
 * Removes successfully killed or already exited (ESRCH) children from the list.
 * Prints actions to stderr.
 *
 * Accepts:
 *   reason - A string indicating why children are being killed (for logging).
 *
 * Returns: None
 */
static void kill_all_children(const char *reason) {
    pid_t parent_pid = getpid();

    if (g_child_count == 0) {


        if (strcmp(reason, "Parent exiting.") != 0) {
            if (printf("PARENT [%d]: No children to kill.\r\n", parent_pid) < 0) { /* Handle error? */ }
        }
        return;
    }


    if (fprintf(stderr, "PARENT [%d]: Killing all %zu children (%s).\r\n", parent_pid, g_child_count, reason) < 0) { /* Handle error? */ }
    if (fflush(stderr) == EOF) { /* Handle error? */ }


    for (size_t i = g_child_count; i > 0; --i) {
        size_t current_index = i - 1;
        pid_t pid_to_kill = g_child_pids[current_index];


        if (fprintf(stderr, "PARENT [%d]: Sending SIGKILL to child PID %d...\r\n", parent_pid, pid_to_kill) < 0) { /* Handle error? */ }
        if (fflush(stderr) == EOF) { /* Handle error? */ }

        int kill_result = kill(pid_to_kill, SIGKILL);

        if (kill_result == -1) {
            if (errno == ESRCH) {
                if (fprintf(stderr, "PARENT [%d]: Child PID %d already exited.\r\n", parent_pid, pid_to_kill) < 0) { /* Handle error? */ }

                remove_child_pid_at_index(current_index);
            } else {
                if (fprintf(stderr, "Warning: Failed to send SIGKILL to PID %d (errno %d: %s).\r\n",
                    pid_to_kill, errno, strerror(errno)) < 0) { /* Handle error? */ }

            }
        } else {


            remove_child_pid_at_index(current_index);
        }



    }


    if (g_child_count == 0) {
        if (fprintf(stderr, "PARENT [%d]: All tracked children processed for killing.\r\n", parent_pid) < 0) { /* Handle error? */ }
    } else {
        if (fprintf(stderr, "PARENT [%d]: Processed children for killing. %zu children remain tracked due to kill errors.\r\n", parent_pid, g_child_count) < 0) { /* Handle error? */ }
    }
    if (fflush(stderr) == EOF) { /* Handle error? */ }
}

/*
 * signal_all_children
 *
 * Sends the specified signal (SIGUSR1 or SIGUSR2) to all tracked children.
 * Reports actions to stdout/stderr. Does NOT remove children on ESRCH here.
 *
 * Accepts:
 *   sig - The signal number to send (SIGUSR1 or SIGUSR2).
 *
 * Returns: None
 */
static void signal_all_children(int sig) {
    pid_t parent_pid = getpid();
    const char *sig_name = (sig == SIGUSR1) ? "SIGUSR1 (enable output)" : "SIGUSR2 (disable output)";

    if (g_child_count == 0) {
        if (printf("PARENT [%d]: No children to send %s to.\r\n", parent_pid, sig_name) < 0) { /* Handle error? */ }
        return;
    }


    if (fprintf(stderr, "PARENT [%d]: Sending %s to all %zu children.\r\n", parent_pid, sig_name, g_child_count) < 0) { /* Handle error? */ }
    if (fflush(stderr) == EOF) { /* Handle error? */ }

    size_t signaled_count = 0;
    size_t esrch_count = 0;
    for (size_t i = 0; i < g_child_count; ++i) {
        pid_t child_pid = g_child_pids[i];
        if (kill(child_pid, sig) == 0) {
            signaled_count++;
        } else {

            if (errno == ESRCH) {
                esrch_count++;


            } else {
                if (fprintf(stderr, "Warning: Failed to send %s to PID %d (errno %d: %s).\r\n",
                    sig_name, child_pid, errno, strerror(errno)) < 0) { /* Handle error? */ }
            }

        }
    }


    if (printf("PARENT [%d]: Attempted to send %s to %zu children. Success: %zu, Already Exited: %zu.\r\n",
        parent_pid, sig_name, g_child_count, signaled_count, esrch_count) < 0) { /* Handle error? */ }
}


/*
 * spawn_child
 *
 * Forks and execs a new child process using the path in g_child_exec_path.
 * Adds the new child's PID to the list. Reports success (stdout) or failure (stderr).
 * Aborts parent on critical failure (add_child_pid).
 *
 * Accepts: None
 * Returns: None
 */
static void spawn_child(void) {
    pid_t pid = fork();

    if (pid == -1) {

        if (fprintf(stderr, "Error: Failed to fork child process (errno %d: %s)\r\n", errno, strerror(errno)) < 0) { /* Handle error? */ }
        return;
    } else if (pid == 0) {


        /* FIX: REMOVED this block to prevent child from resetting terminal mode */
        /*
         *
         *       if (isatty(STDIN_FILENO)) {
         *
         *           tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    }
    */


        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGQUIT, &sa, NULL);
        sigaction(SIGCHLD, &sa, NULL);




        char *const child_argv[] = { g_child_exec_path, NULL };
        execv(g_child_exec_path, child_argv);



        int exec_errno = errno;
        char err_buf[256];
        int len = snprintf(err_buf, sizeof(err_buf), "CHILD: Error: Failed to execute '%s' (errno %d: %s)\n",
                           g_child_exec_path, exec_errno, strerror(exec_errno));
        if (len > 0) {
            safe_write(STDERR_FILENO, err_buf, (size_t)len);
        }
        exit(EXIT_FAILURE);

    } else {


        add_child_pid(pid);

        if (printf("PARENT [%d]: Spawned child process with PID %d. Total children: %zu\r\n",
            getpid(), pid, g_child_count) < 0) { /* Handle error? */ }
    }
}

/*
 * kill_last_child
 *
 * Sends SIGKILL to the most recently spawned child process.
 * Removes the child from the list if kill succeeds or fails with ESRCH.
 * Reports the action (stdout/stderr).
 *
 * Accepts: None
 * Returns: None
 */
static void kill_last_child(void) {
    pid_t parent_pid = getpid();

    if (g_child_count == 0) {
        if (printf("PARENT [%d]: No children to kill.\r\n", parent_pid) < 0) { /* Handle error? */ }
        return;
    }


    size_t last_index = g_child_count - 1;
    pid_t pid_to_kill = g_child_pids[last_index];


    if (fprintf(stderr, "PARENT [%d]: Sending SIGKILL to last child (PID %d).\r\n", parent_pid, pid_to_kill) < 0) { /* Handle error? */ }
    if (fflush(stderr) == EOF) { /* Handle error? */ }

    int kill_result = kill(pid_to_kill, SIGKILL);
    int removed = 0;

    if (kill_result == -1) {
        if (errno == ESRCH) {
            if (fprintf(stderr, "PARENT [%d]: Child PID %d already exited.\r\n", parent_pid, pid_to_kill) < 0) { /* Handle error? */ }

            remove_child_pid_at_index(last_index);
            removed = 1;
        } else {
            if (fprintf(stderr, "Warning: Failed to send SIGKILL to PID %d (errno %d: %s).\r\n",
                pid_to_kill, errno, strerror(errno)) < 0) { /* Handle error? */ }

        }
    } else {


        remove_child_pid_at_index(last_index);
        removed = 1;
    }


    if (removed) {
        if (printf("PARENT [%d]: Removed tracking for child %d. Remaining children: %zu\r\n",
            parent_pid, pid_to_kill, g_child_count) < 0) { /* Handle error? */ }
    } else {
        if (printf("PARENT [%d]: Did not remove tracking for child %d due to kill error. Remaining children: %zu\r\n",
            parent_pid, pid_to_kill, g_child_count) < 0) { /* Handle error? */ }
    }
}

/*
 * list_children
 *
 * Prints the PID of the parent and the PIDs of all currently tracked children to stdout.
 * Uses \r\n for raw mode compatibility. Checks printf return values.
 *
 * Accepts: None
 * Returns: None
 */
static void list_children(void) {
    pid_t parent_pid = getpid();

    char list_buf[4096];
    int current_pos = 0;
    int remaining_buf = sizeof(list_buf);
    int ret;

    ret = snprintf(list_buf + current_pos, remaining_buf, "PARENT [%d]: Listing processes:\r\n", parent_pid);
    if (ret < 0 || ret >= remaining_buf) goto print_error;
    current_pos += ret; remaining_buf -= ret;

    ret = snprintf(list_buf + current_pos, remaining_buf, "  Parent: %d\r\n", parent_pid);
    if (ret < 0 || ret >= remaining_buf) goto print_error;
    current_pos += ret; remaining_buf -= ret;

    if (g_child_count == 0) {
        ret = snprintf(list_buf + current_pos, remaining_buf, "  No tracked children.\r\n");
        if (ret < 0 || ret >= remaining_buf) goto print_error;
        current_pos += ret; remaining_buf -= ret;
    } else {
        ret = snprintf(list_buf + current_pos, remaining_buf, "  Tracked Children (%zu):\r\n", g_child_count);
        if (ret < 0 || ret >= remaining_buf) goto print_error;
        current_pos += ret; remaining_buf -= ret;

        for (size_t i = 0; i < g_child_count; ++i) {

            ret = snprintf(list_buf + current_pos, remaining_buf, "    - PID %d (tracked)\r\n", g_child_pids[i]);
            if (ret < 0 || ret >= remaining_buf) goto print_error;
            current_pos += ret; remaining_buf -= ret;
        }
    }


    if (safe_write(STDOUT_FILENO, list_buf, (size_t)current_pos) == -1) {
        perror("PARENT: Error writing child list");
    }
    return;

    print_error:
    fprintf(stderr, "PARENT [%d]: Error: Buffer overflow while formatting child list.\r\n", parent_pid);

    safe_write(STDOUT_FILENO, list_buf, (size_t)current_pos);
}
