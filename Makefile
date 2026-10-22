CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2 -g
CPPFLAGS += -Ideps/libuv/include -Ideps/llhttp/include -ILuaJIT-2.1/src
LDFLAGS += -Wl,-E

LUAJIT_DIR  = LuaJIT-2.1
UV_DIR      = deps/libuv
LLHTTP_DIR  = deps/llhttp

LUAJIT_LIB  = $(LUAJIT_DIR)/src/libluajit.a
UV_LIB      = $(UV_DIR)/build/libuv.a
LLHTTP_LIB  = $(LLHTTP_DIR)/build/libllhttp.a
LIB_ARCHIVES = $(LUAJIT_LIB) $(UV_LIB) $(LLHTTP_LIB)
LIBS        = $(LIB_ARCHIVES) -pthread -ldl -lrt -lm

SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

all: tmg

tmg: $(OBJ) $(LIB_ARCHIVES)
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LIBS) $(LDFLAGS)

%.o: %.c http.h str.h | deps
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

.PHONY: deps
deps:
	@sh scripts/fetch_deps.sh

$(LUAJIT_LIB):
	$(MAKE) -C $(LUAJIT_DIR) -j$$(nproc)

$(UV_LIB): | deps
	cmake -S $(UV_DIR) -B $(UV_DIR)/build -DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=OFF -DLIBUV_BUILD_SHARED=OFF \
		-DLIBUV_BUILD_TESTS=OFF -DLIBUV_BUILD_BENCH=OFF
	cmake --build $(UV_DIR)/build -j$$(nproc)

$(LLHTTP_LIB): | deps
	cmake -S $(LLHTTP_DIR) -B $(LLHTTP_DIR)/build -DCMAKE_BUILD_TYPE=Release \
		-DLLHTTP_BUILD_SHARED_LIBS=OFF -DLLHTTP_BUILD_STATIC_LIBS=ON
	cmake --build $(LLHTTP_DIR)/build -j$$(nproc)

test: tmg
	python3 tests/test_http.py
	python3 tests/htmx_todo/test.py

clean:
	rm -f $(OBJ) tmg

distclean: clean
	rm -rf $(UV_DIR)/build $(LLHTTP_DIR)/build

.PHONY: all test clean distclean
