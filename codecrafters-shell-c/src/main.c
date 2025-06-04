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
#include <signal.h>

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
      // Skip leading whitespace
      while (isspace((unsigned char)*p)) p++;
      if (*p == '\0') break;

      char buffer[1024];
      int buf_idx = 0;

      // Collect the full argument including any quoted/unquoted segments
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
                          // Preserve other escapes literally inside double quotes
                          buffer[buf_idx++] = *p++;
                      }
                  } else {
                      buffer[buf_idx++] = *p++;
                  }
              }
              if (*p == quote) p++; // Skip closing quote
              else {
                  fprintf(stderr, "parse_input: missing closing %c quote\n", quote);
                  for (int i = 0; i < argc; i++) free(args[i]);
                  return -1;
              }
          } else if (*p == '\\' && *(p + 1)) {
              // Outside quotes: treat \x as x (basic escaping)
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
          for (int i = 0; i < argc; i++) free(args[i]);
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

void sigint_handler(int sig) {
    // Ignore Ctrl+C in shell
    if (isatty(STDIN_FILENO)) {
        write(STDOUT_FILENO, "\n$ ", 3);
    }
}

int main() {
    char input[1024];
    setbuf(stdout, NULL); // Ensure immediate stdout flush

    // Ignore SIGINT in shell
    signal(SIGINT, sigint_handler);

    while (1) {
        if (isatty(STDOUT_FILENO)) {
            fputs("$ ", stdout);
            fflush(stdout);
        }

        if (fgets(input, sizeof(input), stdin) == NULL) break;
        input[strcspn(input, "\n")] = '\0';  // Remove newline

        char *args[MAX_ARGS];
        int arg_count = parse_input(input, args, MAX_ARGS);

        if (arg_count <= 0) continue;

        char *outfile = NULL;
        bool is_redirect = false;
        int saved_stdout = -1;

        // Detect output redirection
        for (int i = 0; i < arg_count - 1; i++) {
            if (strcmp(args[i], ">") == 0 || strcmp(args[i], "1>") == 0) {
                if (i + 1 >= arg_count || !args[i + 1]) {
                    fprintf(stderr, "syntax error: expected file after '%s'\n", args[i]);
                    free_args(args, arg_count);
                    goto next_loop;
                }
                outfile = args[i + 1];

                for (int j = i; j + 2 <= arg_count; j++) {
                    args[j] = args[j + 2];
                }
                arg_count -= 2;
                is_redirect = true;
                break;
            }
        }

        if (is_redirect) {
            saved_stdout = dup(STDOUT_FILENO);
            int outfd = open(outfile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (outfd < 0) {
                fprintf(stderr, "Error opening file %s: %s\n", outfile, strerror(errno));
                free_args(args, arg_count);
                goto next_loop;
            }
            dup2(outfd, STDOUT_FILENO);
            close(outfd);
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
                free(target); // Only free if it was malloc'ed
            }
              break;                          
            }

            default: // external command
            {
                pid_t pid = fork();
                if (pid == 0) {
                    signal(SIGINT, SIG_DFL); // Restore default signal handling in child
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

        if (is_redirect && saved_stdout != -1) {
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
        }

        free_args(args, arg_count);

    next_loop:
        continue;
    }

    return 0;
}
