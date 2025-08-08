CC     = gcc
CFLAGS = -ggdb -Wall -Wextra -pedantic
LIBS   = -llua

SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

all: $(OBJ)
	$(CC) $(OBJ) -ggdb -o tmg $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) tmg
