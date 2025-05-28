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

#define CMD_LST_SIZE 5
#define MAX_ARGS 50

char *cmd_lst[] = {"echo", "exit", "type", "pwd", "cd"};

// Check if input command matches a known command
int is_cmd(char *cmd) {
    for (int i = 0; i < CMD_LST_SIZE; i++) {
        if (strcmp(cmd, cmd_lst[i]) == 0) {
            return i;
        }
    }
    return -1;
}

// Parse input line into arguments, handling quotes and escape characters
int parse_input(char *input, char *args[], int max_args) {
    int argc = 0;
    char *p = input;

    while (*p && argc < max_args - 1) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;

        char buffer[1024] = {0};
        int buf_idx = 0;

        while (*p && !isspace((unsigned char)*p)) {
            if (*p == '\'') {
                p++;
                while (*p && *p != '\'') buffer[buf_idx++] = *p++;
                if (*p == '\'') p++;
                else {
                    fprintf(stderr, "parse_input: missing closing single quote\n");
                    return -1;
                }
            } else if (*p == '"') {
                p++;
                while (*p && *p != '"') buffer[buf_idx++] = *p++;
                if (*p == '"') p++;
                else {
                    fprintf(stderr, "parse_input: missing closing double quote\n");
                    return -1;
                }
            } else {
                buffer[buf_idx++] = *p++;
            }
        }

        args[argc] = malloc(buf_idx + 1);
        if (!args[argc]) {
            fprintf(stderr, "parse_input: memory allocation failed\n");
            return -1;
        }
        memcpy(args[argc], buffer, buf_idx);
        args[argc][buf_idx] = '\0';
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
    setbuf(stdout, NULL); // Flush stdout after each printf

    while (1) {
        // Ensure prompt displays correctly
        if (isatty(STDOUT_FILENO)) {
            fputs("$ ", stdout);
            fflush(stdout);
        }

        if (fgets(input, sizeof(input), stdin) == NULL) break;

        char *args[MAX_ARGS];
        int arg_count = parse_input(input, args, MAX_ARGS);

        if (arg_count <= 0) {
            free_args(args, arg_count);
            continue;
        }

        char *outfile = NULL;
        int outfd = -1;
        bool is_redirect = false;

        // Detect output redirection
        for (int i = 0; i < arg_count - 1; i++) {
            if ((strcmp(args[i], ">") == 0 || strcmp(args[i], "1>") == 0) && args[i + 1]) {
                outfile = args[i + 1];

                // Remove redirection tokens from args[]
                for (int j = i; j + 2 <= arg_count; j++) {
                    args[j] = args[j + 2];
                }
                arg_count -= 2;
                is_redirect = true;
                break;
            }
        }

        int cmd_type = is_cmd(args[0]);

        switch (cmd_type) {
            case 0: // echo
                if (is_redirect) {
                    outfd = open(outfile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
                    if (outfd < 0) {
                        fprintf(stderr, "Error opening file %s: %s\n", outfile, strerror(errno));
                        free_args(args, arg_count);
                        continue;
                    }
                    dup2(outfd, STDOUT_FILENO);
                    close(outfd);
                }
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
                char *target = arg_count >= 2 ? args[1] : getenv("HOME");
                if (!target) {
                    fprintf(stderr, "cd: HOME not set\n");
                    break;
                }

                if (chdir(target) != 0) {
                    fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
                }
                break;
            }

            default: // external command
            {
                pid_t pid = fork();
                if (pid == 0) {
                    if (is_redirect) {
                        outfd = open(outfile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
                        if (outfd < 0) {
                            fprintf(stderr, "Error opening file %s: %s\n", outfile, strerror(errno));
                            exit(1);
                        }
                        dup2(outfd, STDOUT_FILENO);
                        close(outfd);
                    }
                    execvp(args[0], args);
                    fprintf(stderr, "%s: command not found\n", args[0]);
                    exit(127);
                } else if (pid > 0) {
                    int status;
                    waitpid(pid, &status, 0);
                } else {
                    perror("fork failed");
                }
                break;
            }
        }

        free_args(args, arg_count);
    }

    return 0;
}
