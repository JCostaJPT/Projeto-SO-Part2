#include "board.h"
#include "display.h"
#include "debug.h"
#include "protocol.h"
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <sys/stat.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

typedef struct {
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

int thread_shutdown = 0;

int create_backup() {
    // clear the terminal for process transition
    terminal_cleanup();

    pid_t child = fork();

    if(child != 0) {
        if (child < 0) {
            return -1;
        }

        return child;
    } else {
        debug("[%d] Created\n", getpid());

        return 0;
    }
}

void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();     
}

void* ncurses_thread(void *arg) {
    board_t *board = (board_t*) arg;
    sleep_ms(board->tempo / 2);
    while (true) {
        sleep_ms(board->tempo);
        pthread_rwlock_wrlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        screen_refresh(board, DRAW_MENU);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

void* pacman_thread(void *arg) {
    board_t *board = (board_t*) arg;

    pacman_t* pacman = &board->pacmans[0];

    int *retval = malloc(sizeof(int));

    while (true) {
        if(!pacman->alive) {
            *retval = LOAD_BACKUP;
            return (void*) retval;
        }

        sleep_ms(board->tempo * (1 + pacman->passo));

        command_t* play;
        command_t c;
        if (pacman->n_moves == 0) {
            c.command = get_input();

            if(c.command == '\0') {
                continue;
            }

            c.turns = 1;
            play = &c;
        }
        else {
            play = &pacman->moves[pacman->current_move%pacman->n_moves];
        }

        debug("KEY %c\n", play->command);

        // QUIT
        if (play->command == 'Q') {
            *retval = QUIT_GAME;
            return (void*) retval;
        }
        // FORK
        if (play->command == 'G') {
            *retval = CREATE_BACKUP;
            return (void*) retval;
        }

        pthread_rwlock_rdlock(&board->state_lock);

        int result = move_pacman(board, 0, play);
        if (result == REACHED_PORTAL) {
            // Next level
            *retval = NEXT_LEVEL;
            break;
        }

        if(result == DEAD_PACMAN) {
            // Restart from child, wait for child, then quit
            *retval = LOAD_BACKUP;
            break;
        }

        pthread_rwlock_unlock(&board->state_lock);
    }
    pthread_rwlock_unlock(&board->state_lock);
    return (void*) retval;
}

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;

    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }

        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

void* host_thread_func(void *arg) {
    char* fifo_registo = (char*)arg;
    int reg_fd = open(fifo_registo, O_RDONLY);
    if (reg_fd == -1) {
        perror("open reg fifo");
        return NULL;
    }

    // Keep a writer open to avoid EOF when all clients close
    int reg_fd_w = open(fifo_registo, O_WRONLY);
    if (reg_fd_w == -1) {
        perror("open reg fifo writer");
        close(reg_fd);
        return NULL;
    }

    while (true) {
        char message[1 + MAX_PIPE_PATH_LENGTH + MAX_PIPE_PATH_LENGTH];
        ssize_t r = read(reg_fd, message, sizeof(message));
        if (r == 0) {
            // No writer currently; keep waiting
            continue;
        }
        if (r != sizeof(message)) {
            // Ignore incomplete messages
            continue;
        }

        if (message[0] != OP_CODE_CONNECT) {
            continue;
        }

        char req_pipe[41];
        char notif_pipe[41];
        strncpy(req_pipe, message + 1, 40);
        req_pipe[40] = '\0';
        strncpy(notif_pipe, message + 41, 40);
        notif_pipe[40] = '\0';

        // Send response
        int notif_fd = open(notif_pipe, O_WRONLY);
        if (notif_fd == -1) {
            continue;
        }

        char response[2] = {OP_CODE_CONNECT, 0};
        write(notif_fd, response, 2);

        // Send dummy board update with game_over=1
        char board_msg[1 + 4*6 + 10*10]; // OP + 5 ints + dummy 10x10 board
        board_msg[0] = OP_CODE_BOARD;
        int offset = 1;
        *(int*)(board_msg + offset) = 10; offset += 4; // width
        *(int*)(board_msg + offset) = 10; offset += 4; // height
        *(int*)(board_msg + offset) = 1000; offset += 4; // tempo
        *(int*)(board_msg + offset) = 0; offset += 4; // victory
        *(int*)(board_msg + offset) = 1; offset += 4; // game_over
        *(int*)(board_msg + offset) = 0; offset += 4; // points
        for (int i = 0; i < 100; i++) {
            board_msg[offset + i] = ' ';
        }
        write(notif_fd, board_msg, sizeof(board_msg));

        close(notif_fd);
    }

    close(reg_fd_w);
    close(reg_fd);
    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <levels_dir> <max_games> <fifo_registo>\n", argv[0]);
        return -1;
    }

    char* levels_dir = argv[1];
    int max_games = atoi(argv[2]);
    char* fifo_registo = argv[3];

    // Create FIFO de registo (remove stale one first)
    unlink(fifo_registo);
    if (mkfifo(fifo_registo, 0666) == -1) {
        perror("mkfifo");
        return -1;
    }

    // Create thread to handle connections
    pthread_t host_thread;
    pthread_create(&host_thread, NULL, host_thread_func, (void*)fifo_registo);

    // Wait for thread
    pthread_join(host_thread, NULL);

    // Cleanup
    unlink(fifo_registo);

    return 0;
}
