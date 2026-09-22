CC     = gcc
CFLAGS = -Wall -Wextra -pedantic -O2
# LIBS   = -L./lua-5.4.8/src/ -l:liblua.a -lm -I./lua-5.4.8/src/
LIBS   = -L./LuaJIT-2.1/src -l:libluajit.a -lm -I./LuaJIT-2.1/src/

SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

all: $(OBJ)
	$(CC) $(OBJ) -o tmg $(LIBS)

%.o: %.c lualib
	$(CC) $(CFLAGS) $(LIBS) -c $< -o $@

# lualib: ./lua-5.4.8/src/liblua.a
# 	cd ./lua-5.4.8/ && make all

luajit: ./LuaJIT-2.1/libluajit.a
	cd ./LuaJIT-2.1/ && make all

clean:
	rm -f $(OBJ) tmg

.PHONY: lualib
