#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>

#define MAX_LINE 128                 
#define MAX_ARGS (MAX_LINE/2 + 1)

#define MAX_ALIASES 100
#define MAX_ALIAS_NAME_LEN 64
#define MAX_ALIAS_CMD_LEN 256

#define MAX_JOBS 100

/* ---------- ALIAS YAPISI ---------- */
typedef struct {
    int used;
    char name[MAX_ALIAS_NAME_LEN];
    char command[MAX_ALIAS_CMD_LEN];   /* tüm komut + argümanlar */
} Alias;

/* ---------- BACKGROUND JOB YAPISI ---------- */
typedef struct {
    int active;
    pid_t pid;
} Job;

/* ---------- GLOBAL DEĞİŞKENLER ---------- */
static Alias alias_table[MAX_ALIASES];
static Job   jobs[MAX_JOBS];

static pid_t foreground_pid = -1;   /* şu anda foreground’ta çalışan child pid */

/* ---------- ÖN TANIMLAR ---------- */
void setup(char inputBuffer[], char *args[], int *background);

int  is_builtin(char *args[]);
void run_builtin(char *args[]);
void handle_command(char *args[], int background);

/* alias yardımcıları */
void builtin_alias(char *args[]);
void builtin_unalias(char *args[]);
void list_aliases(void);
void expand_alias(char *args[]);

/* job / background yardımcıları */
void add_job(pid_t pid);
void remove_job(pid_t pid);
int  has_active_jobs(void);
void builtin_fg(char *args[]);
void builtin_exit_shell(void);

/* sinyal handler’ları */
void sigtstp_handler(int sig);
void sigchld_handler(int sig);

/* execv + PATH arama */
void exec_with_path(char *cmd, char *argv[]);

/* ============================================================
 *                 KOMUT SATIRI PARSE (setup)
 *  (ödevdeki mainSetup.c’den uyarlanmış)
 * ============================================================ */
void setup(char inputBuffer[], char *args[], int *background)
{
    int length, /* komut satırı uzunluğu */
        i,
        start,
        ct;

    ct = 0;
    start = -1;
    *background = 0;

    length = read(STDIN_FILENO, inputBuffer, MAX_LINE);

    if (length == 0)
        exit(0);            /* ^D */

    if ((length < 0) && (errno != EINTR)) {
        perror("error reading the command");
        exit(-1);
    }

    for (i = 0; i < length; i++) {
        switch (inputBuffer[i]) {
        case ' ':
        case '\t':
            if (start != -1) {
                args[ct] = &inputBuffer[start];
                ct++;
            }
            inputBuffer[i] = '\0';
            start = -1;
            break;

        case '\n':
            if (start != -1) {
                args[ct] = &inputBuffer[start];
                ct++;
            }
            inputBuffer[i] = '\0';
            args[ct] = NULL;
            break;

        default:
            if (start == -1)
                start = i;
            if (inputBuffer[i] == '&') {
                *background = 1;
                if (i > 0)
                    inputBuffer[i-1] = '\0';
            }
            break;
        }
    }

    if (ct == 0)
        args[0] = NULL;
    else
        args[ct] = NULL;

    /* args içinde & varsa kaldır */
    if (*background) {
        for (int j = 0; args[j] != NULL; j++) {
            if (strcmp(args[j], "&") == 0) {
                args[j] = NULL;
                break;
            }
        }
    }
}

/* ============================================================
 *                          MAIN
 * ============================================================ */
int main(void)
{
    char inputBuffer[MAX_LINE];
    int  background;
    char *args[MAX_ARGS];

    /* sinyal handler’ları */
    struct sigaction sa_tstp;
    memset(&sa_tstp, 0, sizeof(sa_tstp));
    sa_tstp.sa_handler = sigtstp_handler;
    sigemptyset(&sa_tstp.sa_mask);
    sa_tstp.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &sa_tstp, NULL);   /* ^Z */

    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);   /* child bittiğinde */

    /* ^C shell’i öldürmesin */
    signal(SIGINT, SIG_IGN);

    while (1) {
        background = 0;
        printf("myshell: ");
        fflush(stdout);

        setup(inputBuffer, args, &background);

        if (args[0] == NULL) {
            continue;
        }

        /* alias genişletme */
        expand_alias(args);

        handle_command(args, background);
    }
}

/* ============================================================
 *                    BUILT-IN KOMUTLAR
 * ============================================================ */

int is_builtin(char *args[])
{
    if (args[0] == NULL) return 0;

    if (strcmp(args[0], "alias") == 0)   return 1;
    if (strcmp(args[0], "unalias") == 0) return 1;
    if (strcmp(args[0], "fg") == 0)      return 1;
    if (strcmp(args[0], "exit") == 0)    return 1;

    return 0;
}

void run_builtin(char *args[])
{
    if (strcmp(args[0], "alias") == 0) {
        builtin_alias(args);
    } else if (strcmp(args[0], "unalias") == 0) {
        builtin_unalias(args);
    } else if (strcmp(args[0], "fg") == 0) {
        builtin_fg(args);
    } else if (strcmp(args[0], "exit") == 0) {
        builtin_exit_shell();
    }
}

/* ---------- alias / unalias ---------- */

void list_aliases(void)
{
    for (int i = 0; i < MAX_ALIASES; i++) {
        if (alias_table[i].used) {
            printf("%s \"%s\"\n", alias_table[i].name, alias_table[i].command);
        }
    }
}

/* alias "ls -l" list
   alias list ls -l (ikisini de destekliyoruz) */
void builtin_alias(char *args[])
{
    if (args[1] && strcmp(args[1], "-l") == 0) {
        list_aliases();
        return;
    }

    if (args[1] == NULL) {
        list_aliases();
        return;
    }

    char name_buf[MAX_ALIAS_NAME_LEN];
    char cmd_buf[MAX_ALIAS_CMD_LEN];
    cmd_buf[0] = '\0';

    /* Eğer ilk argüman tırnakla başlıyorsa: alias "ls -l" list */
    if (args[1][0] == '\"') {
        int i = 1;
        int done = 0;
        for (; args[i] != NULL && !done; i++) {
            char *tok = args[i];
            size_t len = strlen(tok);
            int start = 0, end = (int)len;

            if (i == 1 && tok[0] == '\"') {
                start = 1;
            }
            if (len > 0 && tok[len-1] == '\"') {
                end = (int)len - 1;
                done = 1;
            }

            if (end > start) {
                if (cmd_buf[0] != '\0')
                    strncat(cmd_buf, " ", sizeof(cmd_buf) - strlen(cmd_buf) - 1);
                strncat(cmd_buf, tok + start, end - start);
            }
        }

        if (!done || args[i] == NULL) {
            fprintf(stderr, "Usage: alias \"command with spaces\" name\n");
            return;
        }

        strncpy(name_buf, args[i], sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
    } else {
        /* alias name cmd ... */
        if (args[2] == NULL) {
            fprintf(stderr, "Usage: alias name command\n");
            return;
        }
        strncpy(name_buf, args[1], sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';

        for (int i = 2; args[i] != NULL; i++) {
            if (cmd_buf[0] != '\0')
                strncat(cmd_buf, " ", sizeof(cmd_buf) - strlen(cmd_buf) - 1);
            strncat(cmd_buf, args[i],
                    sizeof(cmd_buf) - strlen(cmd_buf) - 1);
        }
    }

    /* tabloya ekle/güncelle */
    for (int i = 0; i < MAX_ALIASES; i++) {
        if (alias_table[i].used &&
            strcmp(alias_table[i].name, name_buf) == 0) {
            strncpy(alias_table[i].command, cmd_buf,
                    sizeof(alias_table[i].command) - 1);
            alias_table[i].command[sizeof(alias_table[i].command) - 1] = '\0';
            return;
        }
    }

    for (int i = 0; i < MAX_ALIASES; i++) {
        if (!alias_table[i].used) {
            alias_table[i].used = 1;
            strncpy(alias_table[i].name, name_buf,
                    sizeof(alias_table[i].name) - 1);
            alias_table[i].name[sizeof(alias_table[i].name) - 1] = '\0';
            strncpy(alias_table[i].command, cmd_buf,
                    sizeof(alias_table[i].command) - 1);
            alias_table[i].command[sizeof(alias_table[i].command) - 1] = '\0';
            return;
        }
    }

    fprintf(stderr, "myshell: alias table full\n");
}

void builtin_unalias(char *args[])
{
    if (!args[1]) {
        fprintf(stderr, "Usage: unalias name\n");
        return;
    }

    for (int i = 0; i < MAX_ALIASES; i++) {
        if (alias_table[i].used &&
            strcmp(alias_table[i].name, args[1]) == 0) {
            alias_table[i].used = 0;
            alias_table[i].name[0] = '\0';
            alias_table[i].command[0] = '\0';
            return;
        }
    }

    fprintf(stderr, "myshell: alias '%s' not found\n", args[1]);
}

/* alias genişletme: alias ismini gerçek komuta dönüştür */
void expand_alias(char *args[])
{
    if (args[0] == NULL)
        return;

    Alias *a = NULL;
    for (int i = 0; i < MAX_ALIASES; i++) {
        if (alias_table[i].used &&
            strcmp(alias_table[i].name, args[0]) == 0) {
            a = &alias_table[i];
            break;
        }
    }
    if (!a) return;

    static char alias_buf[MAX_ALIAS_CMD_LEN];
    strncpy(alias_buf, a->command, sizeof(alias_buf) - 1);
    alias_buf[sizeof(alias_buf) - 1] = '\0';

    char *new_args[MAX_ARGS];
    int   newc = 0;

    char *saveptr = NULL;
    char *tok = strtok_r(alias_buf, " \t", &saveptr);
    while (tok && newc < MAX_ARGS - 1) {
        new_args[newc++] = tok;
        tok = strtok_r(NULL, " \t", &saveptr);
    }

    for (int i = 1; args[i] != NULL && newc < MAX_ARGS - 1; i++) {
        new_args[newc++] = args[i];
    }
    new_args[newc] = NULL;

    for (int i = 0; i <= newc; i++) {
        args[i] = new_args[i];
    }
}

/* ---------- JOB / BACKGROUND YÖNETİMİ ---------- */

void add_job(pid_t pid)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].active) {
            jobs[i].active = 1;
            jobs[i].pid = pid;
            return;
        }
    }
    fprintf(stderr, "myshell: job table full\n");
}

void remove_job(pid_t pid)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active && jobs[i].pid == pid) {
            jobs[i].active = 0;
            jobs[i].pid = 0;
            return;
        }
    }
}

int has_active_jobs(void)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active)
            return 1;
    }
    return 0;
}

/* fg %pid : background’ı foreground’a al */
void builtin_fg(char *args[])
{
    if (!args[1]) {
        fprintf(stderr, "Usage: fg %%pid\n");
        return;
    }
    if (args[1][0] != '%') {
        fprintf(stderr, "Usage: fg %%pid\n");
        return;
    }

    pid_t pid = (pid_t)atoi(args[1] + 1);
    if (pid <= 0) {
        fprintf(stderr, "myshell: invalid pid\n");
        return;
    }

    int found = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active && jobs[i].pid == pid) {
            found = 1;
            break;
        }
    }
    if (!found) {
        fprintf(stderr, "myshell: no such background job: %d\n", (int)pid);
        return;
    }

    foreground_pid = pid;
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
    }
    foreground_pid = -1;
    remove_job(pid);
}

/* exit builtin */
void builtin_exit_shell(void)
{
    if (has_active_jobs()) {
        fprintf(stderr,
                "myshell: cannot exit, background processes are still running\n");
        return;
    }
    exit(0);
}

/* ============================================================
 *                     SİNYAL HANDLER’LARI
 * ============================================================ */

/* ^Z: foreground sürecini öldür (spec "terminate" dediği için SIGTERM) */
void sigtstp_handler(int sig)
{
    (void)sig;
    if (foreground_pid > 0) {
        if (kill(foreground_pid, SIGTERM) < 0) {
            perror("kill");
        }
    }
}

/* SIGCHLD: biten child’ları topla (özellikle background’lar) */
void sigchld_handler(int sig)
{
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        remove_job(pid);
    }
}

/* ============================================================
 *               execv ile PATH ÜZERİNDEN ÇALIŞTIRMA
 * ============================================================ */

void exec_with_path(char *cmd, char *argv[])
{
    if (strchr(cmd, '/')) {
        execv(cmd, argv);
        return;
    }

    char *path_env = getenv("PATH");
    if (!path_env) {
        execv(cmd, argv);
        return;
    }

    char *path_copy = strdup(path_env);
    if (!path_copy) {
        perror("strdup");
        execv(cmd, argv);
        return;
    }

    char *token = strtok(path_copy, ":");
    char fullpath[PATH_MAX];

    while (token != NULL) {
        snprintf(fullpath, sizeof(fullpath), "%s/%s", token, cmd);
        execv(fullpath, argv);
        token = strtok(NULL, ":");
    }

    free(path_copy);
}

/* ============================================================
 *                KOMUT ÇALIŞTIRMA (builtin / external)
 * ============================================================ */

void handle_command(char *args[], int background)
{
    if (args[0] == NULL)
        return;

    if (is_builtin(args)) {
        run_builtin(args);
        return;
    }

    /* I/O redirect parse */
    char *argv[MAX_ARGS];
    int   argc = 0;

    char *infile  = NULL;
    char *outfile = NULL;
    char *errfile = NULL;
    int   append_out = 0;

    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "<") == 0 && args[i+1]) {
            infile = args[i+1];
            i++;
        } else if (strcmp(args[i], ">") == 0 && args[i+1]) {
            outfile = args[i+1];
            append_out = 0;
            i++;
        } else if (strcmp(args[i], ">>") == 0 && args[i+1]) {
            outfile = args[i+1];
            append_out = 1;
            i++;
        } else if (strcmp(args[i], "2>") == 0 && args[i+1]) {
            errfile = args[i+1];
            i++;
        } else {
            argv[argc++] = args[i];
        }
    }
    argv[argc] = NULL;

    if (argv[0] == NULL)
        return;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        /* CHILD: I/O redirect */
        int fd;
        if (infile) {
            fd = open(infile, O_RDONLY);
            if (fd < 0) {
                perror("open input");
                _exit(1);
            }
            if (dup2(fd, STDIN_FILENO) < 0) {
                perror("dup2 input");
                _exit(1);
            }
            close(fd);
        }

        if (outfile) {
            int flags = O_WRONLY | O_CREAT;
            if (append_out)
                flags |= O_APPEND;
            else
                flags |= O_TRUNC;

            fd = open(outfile, flags, 0644);
            if (fd < 0) {
                perror("open output");
                _exit(1);
            }
            if (dup2(fd, STDOUT_FILENO) < 0) {
                perror("dup2 output");
                _exit(1);
            }
            close(fd);
        }

        if (errfile) {
            fd = open(errfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                perror("open error");
                _exit(1);
            }
            if (dup2(fd, STDERR_FILENO) < 0) {
                perror("dup2 error");
                _exit(1);
            }
            close(fd);
        }

        /* sinyaller çocuğa normal çalışsın */
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);

        exec_with_path(argv[0], argv);

        fprintf(stderr, "myshell: command not found: %s\n", argv[0]);
        _exit(127);
    } else {
        if (background) {
            add_job(pid);
            printf("myshell: Background process started with PID: %d\n", (int)pid);
        } else {
            foreground_pid = pid;
            int status;
            if (waitpid(pid, &status, 0) < 0) {
                perror("waitpid");
            }
            foreground_pid = -1;
        }
    }
}
