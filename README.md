# Custom Linux Shell

A lightweight, custom command-line interface (CLI) implementation in C for Linux systems. This project demonstrates core Operating Systems concepts including process creation, signal handling, and I/O management.

## Features

This shell mimics standard Linux shell behavior with the following capabilities:

* **Command Execution:** Executes standard Linux commands (e.g., `ls`, `ps`, `grep`) by searching the `PATH` environment variable[cite: 13, 98].
* **I/O Redirection:** Supports input and output redirection for file operations[cite: 55, 98]:
    * `>` : Overwrite standard output to a file.
    * `>>`: Append standard output to a file.
    * `<` : Read standard input from a file.
    * `2>`: Redirect standard error to a file.
* **Background Processing:** Runs commands in the background using the `&` operator[cite: 19, 98].
* **Process Management:**
    * [cite_start]Creating new processes using `fork()` and `execv()`[cite: 11, 98].
    * [cite_start]Handling zombie processes with `waitpid()`[cite: 22, 98].
    * [cite_start]`fg %<pid>` command to bring background jobs to the foreground[cite: 51, 98].
* **Built-in Commands:**
    * `alias` / `unalias`: Create and remove custom shortcuts for commands[cite: 24, 98].
    * `exit`: Safely terminates the shell (prevents exit if background tasks are active)[cite: 53, 98].
* **Signal Handling:**
    * `Ctrl+Z` (SIGTSTP): Stops the foreground process[cite: 49, 98].
    * `Ctrl+C` (SIGINT): Ignored by the shell to prevent accidental closure[cite: 98].

## Technical Implementation

The project is built using standard C libraries and Linux system calls:
* `fork()` & `execv()`: To create child processes and execute programs.
* `dup2()`: For redirecting `stdin`, `stdout`, and `stderr`.
* `signal()` & `sigaction()`: To capture and handle OS signals.
* `waitpid()`: To manage background and foreground process states.

## Installation & Usage

### 1. Compile the Shell
Open your terminal and run the following command to compile the source code:

```bash
gcc -o myshell myshell.c
