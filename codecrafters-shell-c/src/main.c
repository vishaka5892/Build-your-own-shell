#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <readline/readline.h>
#include <readline/history.h>

#define CMD_LST_SIZE 5
#define MAX_ARGS 50

char *cmd_lst[] = {"echo", "exit", "type", "pwd", "cd"};
char *builtin_cmds[] = {"echo", "exit", NULL};

// Autocomplete for built-in commands
char *builtin_completion(const char *text, int state) {
    static int list_index, len;
    const char *name;

    if (state == 0) {
        list_index = 0;
        len = strlen(text);
    }

    while ((name = builtin_cmds[list_index++])) {
        if (strncmp(name, text, len) == 0) {
            char *match = malloc(strlen(name) + 2); // include space
            if (!match) return NULL;
            sprintf(match, "%s", name);
            return match;
        }
    }

    return NULL;
}

char **my_completion(const char *text, int start, int end) {
    rl_attempted_completion_function = NULL;
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, builtin_completion);
}

// Check if input command matches a known command
int is_cmd(char *cmd) {
    for (int i = 0; i < CMD_LST_SIZE; i++) {
        if (strcmp(cmd, cmd_lst[i]) == 0) {
            return i;
        }
    }
    return -1;
}

// Parse input line into arguments
int parse_input(char *input, char *args[], int max_args) {
    int argc = 0;
    char *p = input;

    while (*p && argc < max_args - 1) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;

        char buffer[1024];
        int buf_idx = 0;

        while (*p && !isspace((unsigned char)*p)) {
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                while (*p && *p != quote) {
                    if (quote == '"' && *p == '\\' && *(p + 1)) {
                        char next = *(p + 1);
                        if (next == '"' || next == '\\' || next == '$' || next == '`') {
                            buffer[buf_idx++] = next;
                            p += 2;
                        } else {
                            buffer[buf_idx++] = *p++;
                        }
                    } else {
                        buffer[buf_idx++] = *p++;
                    }
                }
                if (*p == quote) p++;
                else {
                    fprintf(stderr, "parse_input: missing closing %c quote\n", quote);
                    return -1;
                }
            } else if (*p == '\\' && *(p + 1)) {
                buffer[buf_idx++] = *(p + 1);
                p += 2;
            } else {
                buffer[buf_idx++] = *p++;
            }
        }

        buffer[buf_idx] = '\0';
        args[argc] = malloc(buf_idx + 1);
        if (!args[argc]) {
            fprintf(stderr, "parse_input: memory allocation failed\n");
            return -1;
        }
        strcpy(args[argc], buffer);
        argc++;
    }

    args[argc] = NULL;
    return argc;
}

// Free dynamically allocated arguments
void free_args(char *args[], int argc) {
    for (int i = 0; i < argc; i++) {
        free(args[i]);
    }
}

int main() {
    char input[1024];
    rl_attempted_completion_function = my_completion;
    setbuf(stdout, NULL);

    while (1) {
        char *line = readline("$ ");
        if (!line) break;

        if (*line) add_history(line);

        strncpy(input, line, sizeof(input) - 1);
        input[sizeof(input) - 1] = '\0';
        free(line);

        char *args[MAX_ARGS];
        int arg_count = parse_input(input, args, MAX_ARGS);

        if (arg_count <= 0) {
            if (arg_count > 0) free_args(args, arg_count);
            continue;
        }

        char *outfile = NULL, *errfile = NULL;
        bool redirect_out = false, redirect_err = false;
        bool append_out = false, append_err = false;
        int saved_stdout = -1, saved_stderr = -1;

        // Updated redirection parsing
        for (int i = 0; i < arg_count; i++) {
            if ((strcmp(args[i], ">") == 0 || strcmp(args[i], "1>") == 0 ||
                 strcmp(args[i], ">>") == 0 || strcmp(args[i], "1>>") == 0 ||
                 strcmp(args[i], "2>") == 0 || strcmp(args[i], "2>>") == 0)) {

                if (i + 1 >= arg_count) {
                    fprintf(stderr, "Redirection operator '%s' missing file operand\n", args[i]);
                    free_args(args, arg_count);
                    return -1;
                }

                char *op = args[i];
                char *filename = args[i + 1];

                if (strcmp(op, ">") == 0 || strcmp(op, "1>") == 0) {
                    outfile = strdup(filename);
                    redirect_out = true;
                    append_out = false;
                } else if (strcmp(op, ">>") == 0 || strcmp(op, "1>>") == 0) {
                    outfile = strdup(filename);
                    redirect_out = true;
                    append_out = true;
                } else if (strcmp(op, "2>") == 0) {
                    errfile = strdup(filename);
                    redirect_err = true;
                    append_err = false;
                } else if (strcmp(op, "2>>") == 0) {
                    errfile = strdup(filename);
                    redirect_err = true;
                    append_err = true;
                }

                free(args[i]);
                free(args[i + 1]);
                for (int j = i; j + 2 < arg_count; j++) {
                    args[j] = args[j + 2];
                }
                arg_count -= 2;
                i--;
            }
        }
        args[arg_count] = NULL;

        // Set up redirection
        if (redirect_out && outfile) {
            saved_stdout = dup(STDOUT_FILENO);
            int flags = O_CREAT | O_WRONLY | (append_out ? O_APPEND : O_TRUNC);
            int outfd = open(outfile, flags, 0644);
            if (outfd < 0) {
                fprintf(stderr, "Error opening file %s: %s\n", outfile, strerror(errno));
                free_args(args, arg_count);
                free(outfile);
                free(errfile);
                continue;
            }
            dup2(outfd, STDOUT_FILENO);
            close(outfd);
        }

        if (redirect_err && errfile) {
            saved_stderr = dup(STDERR_FILENO);
            int flags = O_CREAT | O_WRONLY | (append_err ? O_APPEND : O_TRUNC);
            int errfd = open(errfile, flags, 0644);
            if (errfd < 0) {
                fprintf(stderr, "Error opening file %s: %s\n", errfile, strerror(errno));
                if (redirect_out && saved_stdout != -1) {
                    dup2(saved_stdout, STDOUT_FILENO);
                    close(saved_stdout);
                }
                free_args(args, arg_count);
                free(outfile);
                free(errfile);
                continue;
            }
            dup2(errfd, STDERR_FILENO);
            close(errfd);
        }

        int cmd_type = is_cmd(args[0]);

        switch (cmd_type) {
            case 0: // echo
                for (int i = 1; i < arg_count; i++) {
                    printf("%s", args[i]);
                    if (i < arg_count - 1) printf(" ");
                }
                printf("\n");
                break;

            case 1: // exit
            {
                int status = 0;
                if (arg_count > 1) status = atoi(args[1]);
                free_args(args, arg_count);
                if (outfile) free(outfile);
                if (errfile) free(errfile);
                return status;
            }

            case 2: // type
                if (arg_count < 2) {
                    fprintf(stdout, "type: missing argument\n");
                } else {
                    int builtin = is_cmd(args[1]);
                    if (builtin >= 0) {
                        fprintf(stdout, "%s is a shell builtin\n", args[1]);
                    } else {
                        char *path = getenv("PATH");
                        if (path) {
                            char *path_dup = strdup(path);
                            if (!path_dup) {
                                fprintf(stderr, "type: memory error\n");
                                break;
                            }
                            char *dir = strtok(path_dup, ":");
                            bool found = false;
                            while (dir != NULL) {
                                char full_path[512];
                                snprintf(full_path, sizeof(full_path), "%s/%s", dir, args[1]);
                                if (access(full_path, X_OK) == 0) {
                                    fprintf(stdout, "%s is %s\n", args[1], full_path);
                                    found = true;
                                    break;
                                }
                                dir = strtok(NULL, ":");
                            }
                            if (!found) {
                                fprintf(stdout, "%s: not found\n", args[1]);
                            }
                            free(path_dup);
                        }
                    }
                }
                break;

            case 3: // pwd
            {
                char cwd[512];
                if (getcwd(cwd, sizeof(cwd)) != NULL) {
                    printf("%s\n", cwd);
                } else {
                    perror("pwd");
                }
                break;
            }

            case 4: // cd
            {
                char *target;
                if (arg_count >= 2) {
                    if (args[1][0] == '~') {
                        const char *home = getenv("HOME");
                        if (!home) home = "/";
                        size_t len = strlen(home) + strlen(args[1]);
                        target = malloc(len);
                        if (!target) {
                            fprintf(stderr, "cd: memory allocation error\n");
                            break;
                        }
                        snprintf(target, len, "%s%s", home, args[1] + 1);
                    } else {
                        target = args[1];
                    }
                } else {
                    target = getenv("HOME");
                }

                if (!target) {
                    fprintf(stderr, "cd: HOME not set\n");
                    break;
                }
                if (chdir(target) != 0) {
                    fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
                }
                if (arg_count >= 2 && args[1][0] == '~') {
                    free(target);
                }
                break;
            }

            default: // external command
            {
                pid_t pid = fork();
                if (pid == 0) {
                    execvp(args[0], args);
                    fprintf(stderr, "%s: command not found\n", args[0]);
                    exit(127);
                } else if (pid > 0) {
                    int status;
                    waitpid(pid, &status, 0);
                } else {
                    perror("fork failed");
                }
        }

        }

        // Restore output and error FDs
        if (redirect_out && saved_stdout != -1) {
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
        }
        if (redirect_err && saved_stderr != -1) {
            dup2(saved_stderr, STDERR_FILENO);
            close(saved_stderr);
        }

        free_args(args, arg_count);
        if (outfile) free(outfile);
        if (errfile) free(errfile);
    }

    return 0;
}
