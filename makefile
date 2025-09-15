TARGET = shell.out
SRC_DIR = src
OBJ = $(SRC_DIR)/main.c $(SRC_DIR)/parser.c $(SRC_DIR)/helpers.c $(SRC_DIR)/builtins.c $(SRC_DIR)/excecutor.c $(SRC_DIR)/job_control.c
CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Werror -Wno-unused-parameter -fno-asm -Iinclude
CC = gcc

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

clean:
	rm -f $(SRC_DIR)/*.o

fclean: clean
	rm -f $(TARGET)

re: fclean all