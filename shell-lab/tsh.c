/**
 * @file tsh.c
 * @brief A tiny shell program with job control
 *
 * The main function constantly takes in command line instructions
 * and process them accordingly. The shell supports running
 * a series of builtin commands including quit, job, fg, and bg.
 * It also supports executing foreground and background jobs with
 * I/O redirections. It handles signals and reap programs if they
 * are stopped or terminated.
 *
 *
 * @author Yuxuan Xiao <yuxuanx@andrew.cmu.edu>
 * TODO: Include your name and Andrew ID here.
 */

#include "csapp.h"
#include "tsh_helper.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * If DEBUG is defined, enable contracts and printing on dbg_printf.
 */
#ifdef DEBUG
/* When debugging is enabled, these form aliases to useful functions */
#define dbg_printf(...) printf(__VA_ARGS__)
#define dbg_requires(...) assert(__VA_ARGS__)
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_ensures(...) assert(__VA_ARGS__)
#else
/* When debugging is disabled, no code gets generated for these */
#define dbg_printf(...)
#define dbg_requires(...)
#define dbg_assert(...)
#define dbg_ensures(...)
#endif

/* Function prototypes */
void eval(const char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);
void sigquit_handler(int sig);
void cleanup(void);

/**
 * @brief
 *
 * Iteratively get instructions from command line and process them
 * accordingly.
 *
 * @param[in] argc
 * @param[in] argv
 * @return an integer
 */
int main(int argc, char **argv) {
    char c;
    char cmdline[MAXLINE_TSH]; // Cmdline for fgets
    bool emit_prompt = true;   // Emit prompt (default)

    // Redirect stderr to stdout (so that driver will get all output
    // on the pipe connected to stdout)
    if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
        perror("dup2 error");
        exit(1);
    }

    // Parse the command line
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h': // Prints help message
            usage();
            break;
        case 'v': // Emits additional diagnostic info
            verbose = true;
            break;
        case 'p': // Disables prompt printing
            emit_prompt = false;
            break;
        default:
            usage();
        }
    }

    // Create environment variable
    if (putenv("MY_ENV=42") < 0) {
        perror("putenv error");
        exit(1);
    }

    // Set buffering mode of stdout to line buffering.
    // This prevents lines from being printed in the wrong order.
    if (setvbuf(stdout, NULL, _IOLBF, 0) < 0) {
        perror("setvbuf error");
        exit(1);
    }

    // Initialize the job list
    init_job_list();

    // Register a function to clean up the job list on program termination.
    // The function may not run in the case of abnormal termination (e.g. when
    // using exit or terminating due to a signal handler), so in those cases,
    // we trust that the OS will clean up any remaining resources.
    if (atexit(cleanup) < 0) {
        perror("atexit error");
        exit(1);
    }

    // Install the signal handlers
    Signal(SIGINT, sigint_handler);   // Handles Ctrl-C
    Signal(SIGTSTP, sigtstp_handler); // Handles Ctrl-Z
    Signal(SIGCHLD, sigchld_handler); // Handles terminated or stopped child

    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    Signal(SIGQUIT, sigquit_handler);

    // Execute the shell's read/eval loop
    while (true) {
        if (emit_prompt) {
            printf("%s", prompt);

            // We must flush stdout since we are not printing a full line.
            fflush(stdout);
        }

        if ((fgets(cmdline, MAXLINE_TSH, stdin) == NULL) && ferror(stdin)) {
            perror("fgets error");
            exit(1);
        }

        if (feof(stdin)) {
            // End of file (Ctrl-D)
            printf("\n");
            return 0;
        }

        // Remove any trailing newline
        char *newline = strchr(cmdline, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }

        // Evaluate the command line
        eval(cmdline);
    }

    return -1; // control never reaches here
}

volatile pid_t pid;

/**
 * @brief Parses a command line
 *
 * It processes the command line based on the token returned from the
 * parseline function. It supports builtin commands and runs foreground
 * and background jobs accordingly.
 *
 * NOTE: The shell is supposed to be a long-running process, so this function
 *       (and its helpers) should avoid exiting on error.  This is not to say
 *       they shouldn't detect and print (or otherwise handle) errors!
 */
void eval(const char *cmdline) {
    parseline_return parse_result;
    struct cmdline_tokens token;
    jid_t jid;
    sigset_t mask_all, prev_mask, mask_three;
    sigfillset(&mask_all);
    sigemptyset(&mask_three);
    sigaddset(&mask_three, SIGINT);
    sigaddset(&mask_three, SIGCHLD);
    sigaddset(&mask_three, SIGTSTP);

    // Parse command line
    parse_result = parseline(cmdline, &token);

    if (parse_result == PARSELINE_ERROR || parse_result == PARSELINE_EMPTY) {
        return;
    }

    // Quits shell
    if (token.builtin == BUILTIN_QUIT) {
        exit(0);
    }

    // Built-in job command
    else if (token.builtin == BUILTIN_JOBS) {
        if (token.outfile == NULL) {
            sigprocmask(SIG_BLOCK, &mask_three, &prev_mask);
            list_jobs(STDOUT_FILENO);
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
        } else {
            int fd = open(token.outfile, O_WRONLY | O_CREAT | O_TRUNC,
                          S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
            // Check valid output file
            if (fd < 0) {
                if (errno == EACCES) {
                    printf("%s: Permission denied\n", token.outfile);
                } else {
                    printf("%s: No such file or directory\n", token.outfile);
                }
                return;
            }
            sigprocmask(SIG_BLOCK, &mask_three, &prev_mask);
            list_jobs(fd);
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
            close(fd);
        }
    }

    // Builtin fg
    else if (token.builtin == BUILTIN_FG) {
        // Checks valid input
        if (token.argc < 2) {
            printf("fg command requires PID or %%jobid argument\n");
            return;
        }
        sigprocmask(SIG_BLOCK, &mask_all, &prev_mask);
        if (token.argv[1][0] == '%') { // Input jid
            jid = atoi(token.argv[1] + 1);
            if (jid == 0) {
                printf("fg: argument must be a PID or %%jobid\n");
                sigprocmask(SIG_SETMASK, &prev_mask, NULL);
                return;
            }
            if (!job_exists(jid)) { // Invalid jid
                printf("%s: No such job\n", token.argv[1]);
                sigprocmask(SIG_SETMASK, &prev_mask, NULL);
                return;
            }
            pid = job_get_pid(jid);
        } else { // Input pid
            pid = atoi(token.argv[1]);
            if (pid == 0) {
                printf("fg: argument must be a PID or %%jobid\n");
                sigprocmask(SIG_SETMASK, &prev_mask, NULL);
                return;
            }
            jid = job_from_pid(pid);
            if (jid == 0) { // Invalid pid
                sigprocmask(SIG_SETMASK, &prev_mask, NULL);
                return;
            }
        }
        job_set_state(jid, FG);
        kill(-pid, SIGCONT);
        // Waits for foreground job to finish
        while (fg_job()) {
            sigsuspend(&prev_mask);
        }
        sigprocmask(SIG_SETMASK, &prev_mask, NULL);
    }

    // Builtin bg
    else if (token.builtin == BUILTIN_BG) {
        sigprocmask(SIG_BLOCK, &mask_all, &prev_mask);
        // Checks valid input
        if (token.argc < 2) {
            printf("bg command requires PID or %%jobid argument\n");
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
            return;
        }
        if (token.argv[1][0] == '%') { // Input jid
            jid = atoi(token.argv[1] + 1);
            if (jid == 0) {
                printf("bg: argument must be a PID or %%jobid\n");
                sigprocmask(SIG_SETMASK, &prev_mask, NULL);
                return;
            }
            if (!job_exists(jid)) { // Invalid jid
                printf("%s: No such job\n", token.argv[1]);
                sigprocmask(SIG_SETMASK, &prev_mask, NULL);
                return;
            }
            pid = job_get_pid(jid);
        } else { // Input pid
            pid = atoi(token.argv[1]);
            if (pid == 0) {
                printf("bg: argument must be a PID or %%jobid\n");
                sigprocmask(SIG_SETMASK, &prev_mask, NULL);
                return;
            }
            jid = job_from_pid(pid);
            if (jid == 0) { // Invalid pid
                sigprocmask(SIG_SETMASK, &prev_mask, NULL);
                return;
            }
        }
        job_set_state(jid, BG);
        kill(-pid, SIGCONT);
        printf("[%d] (%d) %s\n", jid, pid, job_get_cmdline(jid));
        sigprocmask(SIG_SETMASK, &prev_mask, NULL);
    }

    else if (token.builtin == BUILTIN_NONE) {
        sigprocmask(SIG_BLOCK, &mask_three, &prev_mask);

        if ((pid = fork()) == 0) {
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
            if (setpgid(pid, pid) != 0) {
                perror("setpgid() error");
            }

            // File redirection
            if (token.infile != NULL) {
                int fd_in = open(token.infile, O_RDONLY, 0);
                if (fd_in < 0) {
                    if (errno == EACCES) {
                        printf("%s: Permission denied\n", token.infile);
                    } else {
                        printf("%s: No such file or directory\n", token.infile);
                    }
                    exit(1);
                }
                dup2(fd_in, STDIN_FILENO);
                close(fd_in);
            }
            if (token.outfile != NULL) {
                int fd_out = open(token.outfile, O_WRONLY | O_CREAT | O_TRUNC,
                                  S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                if (fd_out < 0) {
                    if (errno == EACCES) {
                        printf("%s: Permission denied\n", token.outfile);
                    } else {
                        printf("%s: No such file or directory\n",
                               token.outfile);
                    }
                    exit(1);
                }
                dup2(fd_out, STDOUT_FILENO);
                close(fd_out);
            }

            // Runs the program
            if (execve(token.argv[0], token.argv, environ) < 0) {
                if (errno == EACCES) {
                    printf("%s: Permission denied\n", token.argv[0]);
                } else {
                    printf("%s: No such file or directory\n", token.argv[0]);
                }
                exit(0);
            }
        }

        if (parse_result == PARSELINE_FG) { // Foreground job
            sigprocmask(SIG_BLOCK, &mask_all, NULL);
            add_job(pid, FG, cmdline);
            // Waits for foreground job to finish
            while (fg_job()) {
                sigsuspend(&prev_mask);
            }
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
        } else { // Background job
            sigprocmask(SIG_BLOCK, &mask_all, NULL);
            add_job(pid, BG, cmdline);
            jid = job_from_pid(pid);
            sio_printf("[%d] (%d) %s \n", jid, pid, cmdline);
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
        }
    }

    return;
}

/*****************
 * Signal handlers
 *****************/

/**
 * @brief Handles sigchld signal
 *
 * Reap all children in foreground job
 */
void sigchld_handler(int sig) {
    int olderrno = errno;
    sigset_t mask, prev_mask;
    jid_t jid;
    int status;
    sigfillset(&mask);
    while ((pid = waitpid(-1, &status, WUNTRACED | WNOHANG)) > 0) {
        sigprocmask(SIG_BLOCK, &mask, &prev_mask);
        jid = job_from_pid(pid);
        if (WIFSTOPPED(status)) {
            sio_printf("Job [%d] (%d) stopped by signal %d\n", jid, pid,
                       WSTOPSIG(status));
            job_set_state(jid, ST);
        } else if (WTERMSIG(status)) {
            sio_printf("Job [%d] (%d) terminated by signal %d\n", jid, pid,
                       WTERMSIG(status));
        }
        if (job_get_state(jid) == FG || job_get_state(jid) == BG) {
            delete_job(jid);
        }
        sigprocmask(SIG_SETMASK, &prev_mask, NULL);
    }
    errno = olderrno;
    return;
}

/**
 * @brief handles sigint signal
 *
 * Called when Ctrl-C is pressed, stops the foreground job
 */
void sigint_handler(int sig) {
    int olderrno = errno;
    sigset_t mask, prev_mask;
    sigfillset(&mask);
    sigprocmask(SIG_BLOCK, &mask, &prev_mask);
    jid_t jid = fg_job();
    if (jid > 0) {
        pid = job_get_pid(jid);
        kill(-pid, SIGINT);
    }
    sigprocmask(SIG_SETMASK, &prev_mask, NULL);
    errno = olderrno;
    return;
}

/**
 * @brief Handles sigtstp signal
 *
 * Called when Ctrl-Z is pressed, terminates the foreground job
 */
void sigtstp_handler(int sig) {
    int olderrno = errno;
    sigset_t mask, prev_mask;
    sigfillset(&mask);
    sigprocmask(SIG_BLOCK, &mask, &prev_mask);
    jid_t jid = fg_job();
    if (jid > 0) {
        pid = job_get_pid(jid);
        kill(-pid, SIGTSTP);
    }
    sigprocmask(SIG_SETMASK, &prev_mask, NULL);
    errno = olderrno;
    return;
}

/**
 * @brief Attempt to clean up global resources when the program exits.
 *
 * In particular, the job list must be freed at this time, since it may
 * contain leftover buffers from existing or even deleted jobs.
 */
void cleanup(void) {
    // Signals handlers need to be removed before destroying the joblist
    Signal(SIGINT, SIG_DFL);  // Handles Ctrl-C
    Signal(SIGTSTP, SIG_DFL); // Handles Ctrl-Z
    Signal(SIGCHLD, SIG_DFL); // Handles terminated or stopped child

    destroy_job_list();
}