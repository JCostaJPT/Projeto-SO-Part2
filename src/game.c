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
#include <signal.h>
#include <semaphore.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4
#define MAX_CLIENTS 25
#define BUFFER_SIZE 25

static const char *LEVEL_FILES[] = {"1.lvl", "2.lvl"};
static const int NUM_LEVELS = 2;

typedef struct {
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

typedef struct{
    int client_id;
    int points;
} client_info_t;

typedef struct {
    int req_fd;
    int notif_fd;
    char levels_dir[256];
    int session_id;
    char req_pipe[41];
    char notif_pipe[41];
} session_ctx_t;

typedef struct {
    char fifo_registo[256];
    char levels_dir[256];
    int max_games;
} host_ctx_t;

client_info_t active_clients [MAX_CLIENTS];
int num_active_clients = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

void add_client (int client_id){
    pthread_mutex_lock(&clients_mutex);
    if (num_active_clients < MAX_CLIENTS){
        active_clients[num_active_clients].client_id = client_id;
        active_clients[num_active_clients].points = 0;
        num_active_clients++;
    }
    pthread_mutex_unlock(&clients_mutex);
}

void update_client_points(int client_id, int points){
    pthread_mutex_lock(&clients_mutex);
    for(int i = 0; i < num_active_clients; i++){
        if (active_clients[i].client_id == client_id){
            active_clients[i].points = points;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void remove_client (int client_id){
    pthread_mutex_lock(&clients_mutex);
    for(int i = 0; i < num_active_clients; i++){
        if (active_clients[i].client_id == client_id){
            active_clients[i] = active_clients[--num_active_clients];
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

session_ctx_t *buffer[BUFFER_SIZE];
int buffer_in = 0, buffer_out = 0;
sem_t empty, full;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

int thread_shutdown = 0;
static pthread_mutex_t sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static int active_sessions = 0;
static void dec_sessions(void) {
    pthread_mutex_lock(&sessions_lock);
    active_sessions--;
    pthread_mutex_unlock(&sessions_lock);
}

static int count_remaining_dots(board_t *board) {
    int dots = 0;
    for (int i = 0; i < board->width * board->height; i++) {
        if (board->board[i].has_dot) {
            dots++;
        }
    }
    return dots;
}

static int send_board_update(int notif_fd, board_t *board) {
    int data_size = board->width * board->height;
    int msg_size = 1 + 4 * 6 + data_size;
    char *msg = malloc(msg_size);
    if (!msg) return -1;

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
        int saved = errno;
        perror("write notif board");
        free(msg);
        errno = saved;
        return -1;
    }
    free(msg);
    return 0;
}

static void* session_thread(void *arg) {
    session_ctx_t *ctx = (session_ctx_t *)arg;
    sigset_t mask;
    sigemptyset (&mask);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);      


    int carry_points = 0;

    for (int level_idx = 0; level_idx < NUM_LEVELS; level_idx++) {
        board_t board;
        memset(&board, 0, sizeof(board));

        if (load_level(&board, (char *)LEVEL_FILES[level_idx], ctx->levels_dir, carry_points) != 0) {
            fprintf(stderr, "[server] session %d failed to load level %s\n", ctx->session_id, LEVEL_FILES[level_idx]);
            goto cleanup;
        }

        fprintf(stderr, "[server] session %d level loaded: %s (%dx%d) tempo=%d dots=%d\n",
                ctx->session_id, board.level_name, board.width, board.height, board.tempo, count_remaining_dots(&board));

        // Main session loop for this level
        while (!board.game_over && !board.victory) {
            char buf[32];
            ssize_t n = read(ctx->req_fd, buf, sizeof(buf));
            if (n == 0) {
                board.game_over = 1; // client closed
            } else if (n > 0) {
                fprintf(stderr, "[server] session %d got %zd bytes from client\n", ctx->session_id, n);
                for (ssize_t i = 0; i + 1 < n; i += 2) {
                    if (buf[i] != OP_CODE_PLAY) continue;
                    char cmd = toupper(buf[i + 1]);
                    pthread_rwlock_wrlock(&board.state_lock);
                    move_t res = move_pacman(&board, 0, &(command_t){.command = cmd, .turns = 1, .turns_left = 1});
                    if (res == DEAD_PACMAN) {
                        board.game_over = 1;
                    } else if (res == REACHED_PORTAL) {
                        board.victory = 1;
                    }
                    pthread_rwlock_unlock(&board.state_lock);
                }
            }

            pthread_rwlock_wrlock(&board.state_lock);
            // Move ghosts each tick
            for (int g = 0; g < board.n_ghosts; g++) {
                move_ghost(&board, g, &board.ghosts[g].moves[board.ghosts[g].current_move % board.ghosts[g].n_moves]);
            }
            // Victory check by dots
            if (count_remaining_dots(&board) == 0) {
                board.victory = 1;
            }
            pthread_rwlock_unlock(&board.state_lock);

            pthread_rwlock_rdlock(&board.state_lock);
            if (send_board_update(ctx->notif_fd, &board) == -1 && errno == EPIPE) {
                pthread_rwlock_unlock(&board.state_lock);
                board.game_over = 1;
                break; // client closed pipe
            }
            pthread_rwlock_unlock(&board.state_lock);

            sleep_ms(board.tempo);
        }

        // Finished this level: send final board for this level
        pthread_rwlock_rdlock(&board.state_lock);
        int has_next = (level_idx + 1) < NUM_LEVELS;
        if (board.victory && has_next) {
            board.game_over = 0; // signal transition, not final game over
        } else {
            board.game_over = 1;
        }
        if (send_board_update(ctx->notif_fd, &board) == -1 && errno == EPIPE) {
            pthread_rwlock_unlock(&board.state_lock);
            break;
        }
        pthread_rwlock_unlock(&board.state_lock);

        carry_points = board.accumulated_points;
        unload_level(&board);

        if (board.victory && has_next) {
            continue; // load next level
        }
        break; // either final game over or no more levels
    }

cleanup:
    if (ctx->req_fd != -1) close(ctx->req_fd);
    if (ctx->notif_fd != -1) close(ctx->notif_fd);
    dec_sessions();
    fprintf(stderr, "[server] session %d closed (req=%s notif=%s)\n", ctx->session_id, ctx->req_pipe, ctx->notif_pipe);
    free(ctx);
    return NULL;
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

void* manager_thread_func(void *arg) {
    (void)arg;
    while (true) {
        sem_wait(&full);
        pthread_mutex_lock(&buffer_mutex);
        session_ctx_t *ctx = buffer[buffer_out];
        buffer_out = (buffer_out + 1) % BUFFER_SIZE;
        pthread_mutex_unlock(&buffer_mutex);
        sem_post(&empty);

        // Increment active sessions
        pthread_mutex_lock(&sessions_lock);
        active_sessions++;
        pthread_mutex_unlock(&sessions_lock);

        // Handle the session
        session_thread(ctx);
    }
    return NULL;
}

void* host_thread_func(void *arg) {
    host_ctx_t *host_ctx = (host_ctx_t *)arg;
    char *fifo_registo = host_ctx->fifo_registo;
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

        // Parse client ID from pipe name
        int client_id;
        if (sscanf(req_pipe, "/tmp/%d_request", &client_id) != 1) {
            fprintf(stderr, "[server] invalid pipe name %s\n", req_pipe);
            continue;
        }

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

        session_ctx_t *ctx = calloc(1, sizeof(session_ctx_t));
        if (!ctx) {
            close(req_fd);
            close(notif_fd);
            continue;
        }
        ctx->req_fd = req_fd;
        ctx->notif_fd = notif_fd;
        strncpy(ctx->levels_dir, host_ctx->levels_dir, sizeof(ctx->levels_dir) - 1);
        strncpy(ctx->req_pipe, req_pipe, sizeof(ctx->req_pipe) - 1);
        strncpy(ctx->notif_pipe, notif_pipe, sizeof(ctx->notif_pipe) - 1);
        ctx->session_id = client_id;

        fprintf(stderr, "[server] new session %d: req=%s notif=%s\n", ctx->session_id, req_pipe, notif_pipe);

        // Insert into buffer
        sem_wait(&empty);
        pthread_mutex_lock(&buffer_mutex);
        buffer[buffer_in] = ctx;
        buffer_in = (buffer_in + 1) % BUFFER_SIZE;
        pthread_mutex_unlock(&buffer_mutex);
        sem_post(&full);
    }

    close(reg_fd);
    return NULL;
}

static void sigusr1_handler (int sig){
    (void)sig; //unused parameter

    FILE *log_file = fopen("scores.log","w");
    if (!log_file) return;

    //lock mutex to safely read active clients
    pthread_mutex_lock(&clients_mutex);

    //find top 5 clients
    client_info_t top5[5] = {0};

    for (int i = 0; i < num_active_clients; i++){
        //Compare with top 5 and insert if necessary
        for (int j = 0; j < 5; j++){
            if (active_clients[i].points > top5[j].points){
                //Shift others down
                for (int k = 4; k>j; k--){
                    top5[k] = top5[k-1];
                }
                top5[j] = active_clients [i];
                break;
            }
        }
    }
    //write to file
    fprintf(log_file, "=== TOP 5 SCORES ===\n");
    for (int i = 0; i < 5; i++){
        if (top5[i].client_id != 0){
            fprintf(log_file, "Client %d: %d points\n", top5[i].client_id, top5[i]. points);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    fclose(log_file);
}

int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <levels_dir> <max_games> <fifo_registo>\n", argv[0]);
        return -1;
    }

    char* levels_dir = argv[1];
    int max_games = atoi(argv[2]);
    char* fifo_registo = argv[3];

    fprintf(stderr, "[server] starting, fifo=%s levels_dir=%s max_games=%s\n", fifo_registo, argv[1], argv[2]);

    // Avoid crashing on write to closed FIFOs
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, sigusr1_handler);

    // Create FIFO de registo (remove stale one first)
    unlink(fifo_registo);
    if (mkfifo(fifo_registo, 0666) == -1) {
        perror("mkfifo");
        return -1;
    }

    fprintf(stderr, "[server] fifo created\n");

    // Init semaphores
    sem_init(&empty, 0, BUFFER_SIZE);
    sem_init(&full, 0, 0);

    host_ctx_t *ctx = calloc(1, sizeof(host_ctx_t));
    if (!ctx) {
        fprintf(stderr, "[server] failed to alloc host ctx\n");
        return -1;
    }
    strncpy(ctx->fifo_registo, fifo_registo, sizeof(ctx->fifo_registo) - 1);
    strncpy(ctx->levels_dir, levels_dir, sizeof(ctx->levels_dir) - 1);
    ctx->max_games = max_games;

    // Create manager threads
    pthread_t manager_threads[25];
    for (int i = 0; i < max_games; i++) {
        pthread_create(&manager_threads[i], NULL, manager_thread_func, NULL);
    }

    // Create thread to handle connections
    pthread_t host_thread;
    pthread_create(&host_thread, NULL, host_thread_func, ctx);

    // Wait for thread
    pthread_join(host_thread, NULL);

    free(ctx);

    // Cleanup
    unlink(fifo_registo);

    return 0;
}
