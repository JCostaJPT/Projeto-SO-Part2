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
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <semaphore.h>

#define MAX_CLIENTS 25
#define BUFFER_SIZE 25

typedef struct{
    int client_id;
    int points;
} client_info_t;

typedef struct {
    board_t *board;
    int session_id;
    int stop;
    pthread_mutex_t cmd_lock;
    char pending_cmd;
} session_runtime_t;

static void* pacman_thread(void *arg);
static void* ghost_thread(void *arg);

typedef struct {
    session_runtime_t *runtime;
    int ghost_index;
} ghost_thread_arg_t;

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
client_info_t best_clients[5];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static void update_best_clients(int client_id, int points) {
    for (int i = 0; i < 5; i++) {
        if (best_clients[i].client_id == client_id) {
            if (points > best_clients[i].points) best_clients[i].points = points;
            for (int j = i; j > 0; j--) {
                if (best_clients[j].points > best_clients[j-1].points) {
                    client_info_t tmp = best_clients[j-1];
                    best_clients[j-1] = best_clients[j];
                    best_clients[j] = tmp;
                }
            }
            return;
        }
    }
    if (points <= 0) return;
    for (int i = 0; i < 5; i++) {
        if (best_clients[i].client_id == 0 || points > best_clients[i].points) {
            for (int k = 4; k > i; k--) best_clients[k] = best_clients[k-1];
            best_clients[i].client_id = client_id;
            best_clients[i].points = points;
            break;
        }
    }
}

void add_client (int client_id){
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < num_active_clients; i++) {
        if (active_clients[i].client_id == client_id) {
            active_clients[i].points = 0;
            pthread_mutex_unlock(&clients_mutex);
            return;
        }
    }

    if (num_active_clients < MAX_CLIENTS){
        active_clients[num_active_clients].client_id = client_id;
        active_clients[num_active_clients].points = 0;
        num_active_clients++;
    }
    update_best_clients(client_id, 0);
    pthread_mutex_unlock(&clients_mutex);
}

void update_client_points(int client_id, int points){
    pthread_mutex_lock(&clients_mutex);
    for(int i = 0; i < num_active_clients; i++){
        if (active_clients[i].client_id == client_id){
            active_clients[i].points = points;
            update_best_clients(client_id, points);
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

static pthread_mutex_t sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static int active_sessions = 0;
static pthread_cond_t sessions_cv = PTHREAD_COND_INITIALIZER;
static void dec_sessions(void) {
    pthread_mutex_lock(&sessions_lock);
    active_sessions--;
    pthread_cond_signal(&sessions_cv);
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

static int load_levels_list(const char *levels_dir, char level_files[][256], int max_levels) {
    DIR *d = opendir(levels_dir);
    if (!d) return 0;
    struct dirent *de;
    int count = 0;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        size_t len = strlen(name);
        if (len > 4 && strcmp(name + len - 4, ".lvl") == 0) {
            if (count < max_levels) {
                strncpy(level_files[count], name, 255);
                level_files[count][255] = '\0';
                count++;
            }
        }
    }
    closedir(d);
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(level_files[i], level_files[j]) > 0) {
                char tmp[256];
                strncpy(tmp, level_files[i], 256);
                strncpy(level_files[i], level_files[j], 256);
                strncpy(level_files[j], tmp, 256);
            }
        }
    }
    return count;
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
    pthread_sigmask(SIG_BLOCK, &mask, NULL);      // gameplay threads ignore SIGUSR1


    int carry_points = 0;
    char level_files[MAX_LEVELS][256];
    int num_levels = load_levels_list(ctx->levels_dir, level_files, MAX_LEVELS);
    if (num_levels == 0) {
        fprintf(stderr, "[server] session %d found no levels in %s\n", ctx->session_id, ctx->levels_dir);
        goto cleanup;
    }

    for (int level_idx = 0; level_idx < num_levels; level_idx++) {
        board_t board;
        memset(&board, 0, sizeof(board));

        if (load_level(&board, level_files[level_idx], ctx->levels_dir, carry_points) != 0) {
            fprintf(stderr, "[server] session %d failed to load level %s\n", ctx->session_id, level_files[level_idx]);
            goto cleanup;
        }

        fprintf(stderr, "[server] session %d level loaded: %s (%dx%d) tempo=%d dots=%d\n",
                ctx->session_id, board.level_name, board.width, board.height, board.tempo, count_remaining_dots(&board));

        session_runtime_t rt = {
            .board = &board,
            .session_id = ctx->session_id,
            .stop = 0,
            .pending_cmd = 0
        };
        pthread_mutex_init(&rt.cmd_lock, NULL);

        pthread_t pac_thread;
        pthread_create(&pac_thread, NULL, pacman_thread, &rt);

        pthread_t ghost_threads[MAX_GHOSTS];
        int ghost_thread_count = 0;
        for (int g = 0; g < board.n_ghosts; g++) {
            ghost_thread_arg_t *garg = malloc(sizeof(ghost_thread_arg_t));
            if (!garg) continue;
            garg->runtime = &rt;
            garg->ghost_index = g;
            pthread_create(&ghost_threads[ghost_thread_count++], NULL, ghost_thread, garg);
        }

        while (!rt.stop) {
            char buf[32];
            ssize_t n = read(ctx->req_fd, buf, sizeof(buf));
            if (n == 0) {
                // Client side closed the request pipe
                pthread_rwlock_wrlock(&board.state_lock);
                board.game_over = 1;
                pthread_rwlock_unlock(&board.state_lock);
                rt.stop = 1;
            } else if (n > 0) {
                // Commands arrive as opcode + payload pairs
                for (ssize_t i = 0; i + 1 < n; i += 2) {
                    if (buf[i] == OP_CODE_PLAY) {
                        char cmd = toupper(buf[i + 1]);
                        pthread_mutex_lock(&rt.cmd_lock);
                        rt.pending_cmd = cmd;
                        pthread_mutex_unlock(&rt.cmd_lock);
                    } else if (buf[i] == OP_CODE_DISCONNECT) {
                        pthread_rwlock_wrlock(&board.state_lock);
                        board.game_over = 1;
                        pthread_rwlock_unlock(&board.state_lock);
                        rt.stop = 1;
                    }
                }
            }

            pthread_rwlock_rdlock(&board.state_lock);
            int points_snapshot = board.accumulated_points;
            int victory = board.victory;
            int game_over = board.game_over;
            if (send_board_update(ctx->notif_fd, &board) == -1 && errno == EPIPE) {
                pthread_rwlock_unlock(&board.state_lock);
                rt.stop = 1;
                break; // client closed pipe
            }
            pthread_rwlock_unlock(&board.state_lock);

            update_client_points(ctx->session_id, points_snapshot);

            if (victory || game_over) {
                rt.stop = 1;
                break;
            }

            sleep_ms(board.tempo);
        }

        rt.stop = 1;
        pthread_join(pac_thread, NULL);
        for (int g = 0; g < ghost_thread_count; g++) {
            pthread_join(ghost_threads[g], NULL);
        }

        pthread_rwlock_rdlock(&board.state_lock);
        int has_next = (level_idx + 1) < num_levels;
        if (board.victory && has_next) {
            board.game_over = 0; // signal transition, not final game over
        } else {
            board.game_over = 1;
        }
        int points_snapshot = board.accumulated_points;
        if (send_board_update(ctx->notif_fd, &board) == -1 && errno == EPIPE) {
            pthread_rwlock_unlock(&board.state_lock);
        } else {
            pthread_rwlock_unlock(&board.state_lock);
        }

        update_client_points(ctx->session_id, points_snapshot);

        carry_points = board.accumulated_points;
        pthread_mutex_destroy(&rt.cmd_lock);
        unload_level(&board);

        if (board.victory && has_next) {
            continue; // load next level
        }
        break; // either final game over or no more levels
    }

cleanup:
    if (ctx->req_fd != -1) close(ctx->req_fd);
    if (ctx->notif_fd != -1) close(ctx->notif_fd);
    remove_client(ctx->session_id);
    dec_sessions();
    fprintf(stderr, "[server] session %d closed (req=%s notif=%s)\n", ctx->session_id, ctx->req_pipe, ctx->notif_pipe);
    free(ctx);
    return NULL;
}

void* pacman_thread(void *arg) {
    session_runtime_t *rt = (session_runtime_t *)arg;
    board_t *board = rt->board;
    pacman_t *pacman = &board->pacmans[0];

    while (!rt->stop) {
        sleep_ms(board->tempo * (1 + pacman->passo));

        pthread_rwlock_wrlock(&board->state_lock);
        if (rt->stop || board->game_over || board->victory || !pacman->alive) {
            pthread_rwlock_unlock(&board->state_lock);
            break;
        }

        command_t *play;
        command_t c;
        if (pacman->n_moves == 0) {
            pthread_mutex_lock(&rt->cmd_lock);
            char cmd = rt->pending_cmd;
            rt->pending_cmd = 0;
            pthread_mutex_unlock(&rt->cmd_lock);

            if (cmd == '\0') {
                pthread_rwlock_unlock(&board->state_lock);
                continue;
            }
            if (cmd == 'Q') {
                board->game_over = 1;
                rt->stop = 1;
                pthread_rwlock_unlock(&board->state_lock);
                break;
            }

            // Manual control: build a single-move command on the fly
            c.command = cmd;
            c.turns = 1;
            c.turns_left = 1;
            play = &c;
        } else {
            play = &pacman->moves[pacman->current_move % pacman->n_moves];
        }

        int result = move_pacman(board, 0, play);
        if (result == REACHED_PORTAL) {
            board->victory = 1;
            rt->stop = 1;
        } else if (result == DEAD_PACMAN) {
            board->game_over = 1;
            rt->stop = 1;
        } else if (!board->victory && !board->game_over && count_remaining_dots(board) == 0) {
            board->victory = 1;
            rt->stop = 1;
        }
        pthread_rwlock_unlock(&board->state_lock);
    }
    return NULL;
}

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    session_runtime_t *rt = ghost_arg->runtime;
    int ghost_ind = ghost_arg->ghost_index;
    board_t *board = rt->board;
    free(ghost_arg);

    while (!rt->stop) {
        ghost_t *ghost = &board->ghosts[ghost_ind];
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_wrlock(&board->state_lock);
        if (rt->stop || board->game_over || board->victory) {
            pthread_rwlock_unlock(&board->state_lock);
            break;
        }

        int res = move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move % ghost->n_moves]);
        if (res == DEAD_PACMAN) {
            board->game_over = 1;
            rt->stop = 1;
        }
        pthread_rwlock_unlock(&board->state_lock);
    }
    return NULL;
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

        // Handle the session pulled from the queue
        session_thread(ctx);
    }
    return NULL;
}

void* host_thread_func(void *arg) {
    host_ctx_t *host_ctx = (host_ctx_t *)arg;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
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

        pthread_mutex_lock(&sessions_lock);
        while (active_sessions >= host_ctx->max_games) {
            pthread_cond_wait(&sessions_cv, &sessions_lock);
        }
        pthread_mutex_unlock(&sessions_lock);

        int notif_fd = open(notif_pipe, O_WRONLY);
        if (notif_fd == -1) {
            continue;
        }
        int req_fd = open(req_pipe, O_RDONLY | O_NONBLOCK);
        if (req_fd == -1) {
            close(notif_fd);
            continue;
        }

        add_client(client_id);

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

    client_info_t top5[5] = {0};
    for (int i = 0; i < num_active_clients; i++) {
        for (int j = 0; j < 5; j++) {
            if (active_clients[i].points > top5[j].points) {
                for (int k = 4; k > j; k--) top5[k] = top5[k-1];
                top5[j] = active_clients[i];
                break;
            }
        }
    }

        fprintf(log_file, "=== TOP 5 CLIENTS ===\n");
        for (int i = 0; i < 5; i++) {
            if (best_clients[i].client_id != 0) {
                fprintf(log_file, "Client %d: %d points\n", best_clients[i].client_id, best_clients[i].points);
            }
        }

    pthread_mutex_unlock(&clients_mutex);
    fclose(log_file); // log done
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

    // Block SIGUSR1 in all threads by default; host thread will unblock it
    sigset_t block_all;
    sigemptyset(&block_all);
    sigaddset(&block_all, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block_all, NULL);

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
