#include "board.h"
#include "display.h"
#include "debug.h"
#include "protocol.h"
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdbool.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>

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

static int count_remaining_dots(board_t *board) {
    int dots = 0;
    for (int i = 0; i < board->width * board->height; i++) {
        if (board->board[i].has_dot) {
            dots++;
        }
    }
    return dots;
}

static void send_board_update(int notif_fd, board_t *board) {
    int data_size = board->width * board->height;
    int msg_size = 1 + 4 * 6 + data_size;
    char *msg = malloc(msg_size);
    if (!msg) return;

    msg[0] = OP_CODE_BOARD;
    int offset = 1;
    *(int *)(msg + offset) = board->width; offset += 4;
    *(int *)(msg + offset) = board->height; offset += 4;
    *(int *)(msg + offset) = board->tempo; offset += 4;
    *(int *)(msg + offset) = board->victory; offset += 4;
    *(int *)(msg + offset) = board->game_over; offset += 4;
    *(int *)(msg + offset) = board->accumulated_points; offset += 4;

    // Serialize the board as display-ready chars so the client can show dots/portals
    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int idx = y * board->width + x;

            char out = ' ';

            // Ghosts have priority over dots/portal for drawing
            for (int g = 0; g < board->n_ghosts; g++) {
                ghost_t *gh = &board->ghosts[g];
                if (gh->pos_x == x && gh->pos_y == y) {
                    out = gh->charged ? 'G' : 'M';
                    goto cell_done;
                }
            }

            // Pacman next
            for (int p = 0; p < board->n_pacmans; p++) {
                pacman_t *pc = &board->pacmans[p];
                if (pc->alive && pc->pos_x == x && pc->pos_y == y) {
                    out = 'C';
                    goto cell_done;
                }
            }

            // Static tiles
            if (board->board[idx].content == 'W') {
                out = '#';
            } else if (board->board[idx].has_portal) {
                out = '@';
            } else if (board->board[idx].has_dot) {
                out = '.';
            } else {
                out = ' ';
            }

cell_done:
            msg[offset + idx] = out;
        }
    }

    ssize_t w = write(notif_fd, msg, msg_size);
    if (w != msg_size) {
        perror("write notif board");
    }
    free(msg);
}

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
    // Open FIFO in RDWR to keep both ends open and avoid ENXIO
    int reg_fd = open(fifo_registo, O_RDWR);
    if (reg_fd == -1) {
        perror("open reg fifo");
        return NULL;
    }

    fprintf(stderr, "[server] host ready (listening on %s)\n", fifo_registo);

    while (true) {
        char message[1 + MAX_PIPE_PATH_LENGTH + MAX_PIPE_PATH_LENGTH];
        ssize_t r = read(reg_fd, message, sizeof(message));
        if (r <= 0) {
            if (r < 0) perror("read reg fifo");
            else fprintf(stderr, "[server] reg fifo closed by writer?\n");
            continue;
        }
        fprintf(stderr, "[server] read %zd bytes from reg fifo\n", r);
        if (r != sizeof(message)) {
            // Ignore incomplete messages
            fprintf(stderr, "[server] ignoring incomplete message (%zd bytes)\n", r);
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

        int notif_fd = open(notif_pipe, O_WRONLY);
        if (notif_fd == -1) {
            continue;
        }
        int req_fd = open(req_pipe, O_RDONLY | O_NONBLOCK);
        if (req_fd == -1) {
            close(notif_fd);
            continue;
        }

        char response[2] = {OP_CODE_CONNECT, 0};
        write(notif_fd, response, 2);

        fprintf(stderr, "[server] new session: req=%s notif=%s\n", req_pipe, notif_pipe);

        board_t board;
        memset(&board, 0, sizeof(board));

        // Load first level in directory (default 1.lvl)
        if (load_level(&board, "1.lvl", "levels", 0) != 0) {
            close(req_fd);
            close(notif_fd);
            continue;
        }

        fprintf(stderr, "[server] level loaded: %s (%dx%d) tempo=%d dots=%d\n",
            board.level_name, board.width, board.height, board.tempo, count_remaining_dots(&board));

        // Main session loop
        while (!board.game_over && !board.victory) {
            char buf[32];
            ssize_t n = read(req_fd, buf, sizeof(buf));
            if (n == 0) {
                board.game_over = 1; // client closed
            } else if (n > 0) {
                fprintf(stderr, "[server] got %zd bytes from client\n", n);
                for (ssize_t i = 0; i + 1 < n; i += 2) {
                    if (buf[i] != OP_CODE_PLAY) continue;
                    char cmd = toupper(buf[i + 1]);
                    pthread_rwlock_wrlock(&board.state_lock);
                    move_t res = move_pacman(&board, 0, &(command_t){.command = cmd, .turns = 1, .turns_left = 1});
                    if (res == DEAD_PACMAN) {
                        board.game_over = 1;
                    }
                    pthread_rwlock_unlock(&board.state_lock);
                }
            }

            pthread_rwlock_wrlock(&board.state_lock);
            // Move ghosts each tick
            for (int g = 0; g < board.n_ghosts; g++) {
                move_ghost(&board, g, &board.ghosts[g].moves[board.ghosts[g].current_move % board.ghosts[g].n_moves]);
            }
            // Victory check
            if (count_remaining_dots(&board) == 0) {
                board.victory = 1;
                board.game_over = 1;
            }
            pthread_rwlock_unlock(&board.state_lock);

            pthread_rwlock_rdlock(&board.state_lock);
            send_board_update(notif_fd, &board);
            pthread_rwlock_unlock(&board.state_lock);

            sleep_ms(board.tempo);
        }

        pthread_rwlock_rdlock(&board.state_lock);
        board.game_over = 1;
        send_board_update(notif_fd, &board);
        pthread_rwlock_unlock(&board.state_lock);

        close(req_fd);
        close(notif_fd);
        unload_level(&board);
    }

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

    (void)levels_dir;
    (void)max_games;

    fprintf(stderr, "[server] starting, fifo=%s levels_dir=%s max_games=%s\n", fifo_registo, argv[1], argv[2]);

    // Create FIFO de registo (remove stale one first)
    unlink(fifo_registo);
    if (mkfifo(fifo_registo, 0666) == -1) {
        perror("mkfifo");
        return -1;
    }

    fprintf(stderr, "[server] fifo created\n");

    // Create thread to handle connections
    pthread_t host_thread;
    pthread_create(&host_thread, NULL, host_thread_func, (void*)fifo_registo);

    // Wait for thread
    pthread_join(host_thread, NULL);

    // Cleanup
    unlink(fifo_registo);

    return 0;
}
