#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>


struct Session {
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1};

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
  if (session.id != -1) return 1; // already connected

  fprintf(stderr, "[client] connecting via %s (req=%s notif=%s)\n", server_pipe_path, req_pipe_path, notif_pipe_path);

  // Remove existing FIFOs if any
  unlink(req_pipe_path);
  unlink(notif_pipe_path);

  // Create FIFOs
  if (mkfifo(req_pipe_path, 0666) == -1) { perror("mkfifo req"); return 1; }
  if (mkfifo(notif_pipe_path, 0666) == -1) { perror("mkfifo notif"); return 1; }

  // Open server pipe for writing (non-blocking retry if server not yet listening)
  int server_fd = -1;
  fprintf(stderr, "[client] opening server pipe (nonblock)...\n");
  for (int attempt = 0; attempt < 100; attempt++) {
    server_fd = open(server_pipe_path, O_WRONLY | O_NONBLOCK);
    if (server_fd != -1) break;
    if (errno == ENXIO || errno == ENOENT) {
      sleep_ms(50);
      continue;
    }
    perror("open server pipe");
    return 1;
  }
  if (server_fd == -1) {
    fprintf(stderr, "[client] failed to open server pipe after retries\n");
    return 1;
  }
  fprintf(stderr, "[client] server pipe opened\n");

  // Prepare message
  char message[1 + MAX_PIPE_PATH_LENGTH + MAX_PIPE_PATH_LENGTH];
  message[0] = OP_CODE_CONNECT;
  strncpy(message + 1, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  strncpy(message + 1 + MAX_PIPE_PATH_LENGTH, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
  // Null pad
  for (int i = strlen(req_pipe_path); i < MAX_PIPE_PATH_LENGTH; i++) message[1 + i] = '\0';
  for (int i = strlen(notif_pipe_path); i < MAX_PIPE_PATH_LENGTH; i++) message[1 + MAX_PIPE_PATH_LENGTH + i] = '\0';

  // Send request
  ssize_t w = write(server_fd, message, sizeof(message));
  if (w == -1) {
    close(server_fd);
    return 1;
  }
  fprintf(stderr, "[client] sent %zd bytes connect msg\n", w);
  close(server_fd);

  // Open notification pipe for reading (blocking; will wait for server writer)
  int notif_fd = -1;
  for (int attempt = 0; attempt < 100; attempt++) {
    notif_fd = open(notif_pipe_path, O_RDONLY);
    if (notif_fd != -1) break;
    if (errno == ENOENT) {
      sleep_ms(50);
      continue;
    }
    perror("open notif pipe");
    return 1;
  }
  if (notif_fd == -1) {
    fprintf(stderr, "[client] failed to open notif pipe after retries\n");
    return 1;
  }
  fprintf(stderr, "[client] notif pipe opened\n");

  // Read response
  char response[2];
  ssize_t r = read(notif_fd, response, 2);
  fprintf(stderr, "[client] read connect resp bytes=%zd code=%d res=%d\n", r, response[0], response[1]);
  if (r != 2 || response[0] != OP_CODE_CONNECT || response[1] != 0) {
    close(notif_fd);
    perror("read connect response");
    return 1;
  }

  // Open request pipe for writing
  int req_fd = open(req_pipe_path, O_WRONLY);
  if (req_fd == -1) {
    perror("open req pipe");
    close(notif_fd);
    return 1;
  }

  // Set session
  session.req_pipe = req_fd;
  session.notif_pipe = notif_fd;
  strcpy(session.req_pipe_path, req_pipe_path);
  strcpy(session.notif_pipe_path, notif_pipe_path);
  // Dummy id, parse from path later
  session.id = 1;

  return 0;
}

void pacman_play(char command) {
  if (session.id == -1) return; // not connected

  char message[2];
  message[0] = OP_CODE_PLAY;
  message[1] = command;

  write(session.req_pipe, message, 2);
}

int pacman_disconnect() {
  if (session.id == -1) return 1; // not connected

  char message[1];
  message[0] = OP_CODE_DISCONNECT;
  write(session.req_pipe, message, 1);

  close(session.req_pipe);
  close(session.notif_pipe);

  unlink(session.req_pipe_path);
  unlink(session.notif_pipe_path);

  session.id = -1;
  session.req_pipe = -1;
  session.notif_pipe = -1;

  return 0;
}

Board receive_board_update(void) {
    Board board = {0};
    board.data = NULL;

    if (session.id == -1) return board; // not connected

    char header[1 + 4*6]; // OP + 5 ints + points int
    int bytes_read = read(session.notif_pipe, header, sizeof(header));
    if (bytes_read != sizeof(header) || header[0] != OP_CODE_BOARD) {
      perror("read notif header");
      return board;
    }

    int offset = 1;
    board.width = *(int*)(header + offset); offset += 4;
    board.height = *(int*)(header + offset); offset += 4;
    board.tempo = *(int*)(header + offset); offset += 4;
    board.victory = *(int*)(header + offset); offset += 4;
    board.game_over = *(int*)(header + offset); offset += 4;
    board.accumulated_points = *(int*)(header + offset); offset += 4;

    int data_size = board.width * board.height;
    board.data = malloc(data_size);
    if (!board.data) return board;

    if (read(session.notif_pipe, board.data, data_size) != data_size) {
        free(board.data);
        board.data = NULL;
        return board;
    }

    return board;
}