# Compiler variables
CC = gcc
CFLAGS = -g -Wall -Wextra -std=c17 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lncurses -ltinfo

# Directory variables
OBJ_DIR = obj
BIN_DIR = bin
INCLUDE_DIR = include
CLIENT_DIR = src/client

# executable 
TARGET = Pacmanist

#client
CLIENT = client

#Server objects
OBJS_SERVER = game.o board.o parser.o display.o

#Client objects (use dedicated client display implementation)
OBJS_CLIENT = client_main.o debug.o api.o client_display.o

# Dependencies
display.o = display.h
client_display.o = display.h api.h
board.o = board.h
parser.o = parser.h
api.o = api.h protocol.h

# Object files path
vpath %.o $(OBJ_DIR)
vpath %.c src $(CLIENT_DIR) $(INCLUDE_DIR)

# Make targets
all: client server

client: $(BIN_DIR)/$(CLIENT)

server: $(BIN_DIR)/$(TARGET)

$(BIN_DIR)/$(CLIENT): $(OBJS_CLIENT) | folders
	$(CC) $(CFLAGS) $(addprefix $(OBJ_DIR)/,$(OBJS_CLIENT)) -o $@ $(LDFLAGS)

$(BIN_DIR)/$(TARGET): $(OBJS_SERVER) | folders
	$(CC) $(CFLAGS) $(addprefix $(OBJ_DIR)/,$(OBJS_SERVER)) -o $@ $(LDFLAGS) -lpthread

# dont include LDFLAGS in the end, to allow compilation on macos
%.o: %.c $($@) | folders
	$(CC) -I $(INCLUDE_DIR) $(CFLAGS) -o $(OBJ_DIR)/$@ -c $<

# Explicit rule so client display builds from src/client/display.c
client_display.o: $(CLIENT_DIR)/display.c $($@)
	$(CC) -I $(INCLUDE_DIR) $(CFLAGS) -o $(OBJ_DIR)/$@ -c $<

# Create folders
folders:
	mkdir -p $(OBJ_DIR)
	mkdir -p $(BIN_DIR)

# Clean object files and executable
clean:
	rm -f $(OBJ_DIR)/*.o
	rm -f $(BIN_DIR)/$(TARGET)
	rm -f $(BIN_DIR)/$(CLIENT)

# indentify targets that do not create files
.PHONY: all clean run folders
