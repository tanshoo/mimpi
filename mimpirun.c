// /**
//  * This file is for implementation of mimpirun program.
//  * */

#include "mimpi_common.h"
#include "channel.h"
#include <sys/wait.h>

int main(int argc, char **argv) { // (mimpirun.c), n, prog, args

    // not enough arguments
    if (argc < 3) {
        return -1;
    }

    char n = (char)atoi(argv[1]);

    ASSERT_SYS_OK(setenv(MIMPI_WORLD_VAR, argv[1], 1));

    char **rank = malloc(sizeof(char*)*n);
    for (int i = 0; i < n; i++) {
        rank[i] = malloc(sizeof(char)*4);
        snprintf(rank[i], 4, "%d", i);
    }

    // create channels
    int ch_counter = 20;
    int ch_desc[n][n][2];
    int pipe_desc[2];

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (i == j) {continue;}
            ASSERT_SYS_OK(channel(pipe_desc));
            for (int k = 0; k <= 1; k++) {
                if (pipe_desc[k] != ch_counter) {
                    ASSERT_SYS_OK(dup2(pipe_desc[k], ch_counter));
                    ASSERT_SYS_OK(close(pipe_desc[k]));
                }
                ch_desc[i][j][k] = ch_counter;
                ch_counter++;
            }
        }
    }

    for (int i = 0; i < n; i++) {
        pid_t pid;
        ASSERT_SYS_OK(pid = fork());

        if (!pid) {
            // close useless pipes
            for (int j = 0; j < n; j++) {
                if (j == i) {continue;}
                ASSERT_SYS_OK(close(ch_desc[i][j][0]));
                ASSERT_SYS_OK(close(ch_desc[j][i][1]));
                
                for (int k = 0; k < n; k++) {
                    if (k == i || k == j) {continue;}
                    ASSERT_SYS_OK(close(ch_desc[j][k][0]));
                    ASSERT_SYS_OK(close(ch_desc[j][k][1]));
                }
            }

            // set env variables with pipes
            for (int j = 0; j < n; j++) {
                if (i == j) {continue;}

                char *read_var = malloc(sizeof(char)*32);
                snprintf(read_var, 32, "MIMPI_READ_PIPE_%d", j);
                char *read_desc = malloc(sizeof(char)*5);
                snprintf(read_desc, 5, "%d", ch_desc[j][i][0]);
                ASSERT_SYS_OK(setenv(read_var, read_desc, 1));
                free(read_var);
                free(read_desc);


                char *write_var = malloc(sizeof(char)*32);
                snprintf(write_var, 32, "MIMPI_WRITE_PIPE_%d", j);
                char *write_desc = malloc(sizeof(char)*5);
                snprintf(write_desc, 5, "%d", ch_desc[i][j][1]);
                ASSERT_SYS_OK(setenv(write_var, write_desc, 1));
                free(write_var);
                free(write_desc);
            }

            // assign world rank
            ASSERT_SYS_OK(setenv(MIMPI_RANK_VAR, rank[i], 1));
            ASSERT_SYS_OK(execvp(argv[2], &argv[2]));
        }
    }
    
    // close all pipes
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (i == j) {continue;}
            ASSERT_SYS_OK(close(ch_desc[i][j][0]));
            ASSERT_SYS_OK(close(ch_desc[i][j][1]));
        }
    }
    ASSERT_SYS_OK(unsetenv(MIMPI_WORLD_VAR));

    for (int i = 0; i < n; i++) {
        ASSERT_SYS_OK(wait(NULL));
    }

    for (int i = 0; i < n; i++) {
        free(rank[i]);
    }
    free(rank);
    
    return 0;
}