CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2 -g
CPPFLAGS += -Ideps/libuv/include -Ideps/llhttp/include -Ideps/wslay/lib/includes -ILuaJIT-2.1/src
CPPFLAGS += -DWSLAY_VERSION=\"1.1.1\"
LDFLAGS += -Wl,-E

LUAJIT_DIR  = LuaJIT-2.1
UV_DIR      = deps/libuv
LLHTTP_DIR  = deps/llhttp
WSLAY_DIR   = deps/wslay

LUAJIT_LIB  = $(LUAJIT_DIR)/src/libluajit.a
UV_LIB      = $(UV_DIR)/build/libuv.a
LLHTTP_LIB  = $(LLHTTP_DIR)/build/libllhttp.a
WSLAY_LIB   = $(WSLAY_DIR)/build/lib/libwslay.a
LIB_ARCHIVES = $(LUAJIT_LIB) $(UV_LIB) $(LLHTTP_LIB) $(WSLAY_LIB)
LIBS        = $(LIB_ARCHIVES) -pthread -ldl -lrt -lm

SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

all: tmg

tmg: $(OBJ) $(LIB_ARCHIVES)
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LIBS) $(LDFLAGS)

%.o: %.c http.h str.h | deps
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

http_ws.o: $(WSLAY_LIB)

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

$(WSLAY_LIB): | deps
	cmake -S $(WSLAY_DIR) -B $(WSLAY_DIR)/build -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DWSLAY_STATIC=ON \
		-DWSLAY_SHARED=OFF -DWSLAY_EXAMPLES=OFF -DWSLAY_TESTS=OFF \
		-DWSLAY_CONFIGURE_INSTALL=OFF
	cmake --build $(WSLAY_DIR)/build -j$$(nproc)

test: tmg
	python3 tests/test_http.py
	python3 tests/test_boundaries.py
	python3 tests/htmx_todo/test.py
	python3 tests/react_counter/test.py
	./tmg tests/test_template.lua
	python3 tests/test_template_sep.py
	./tmg tests/test_json.lua
	./tmg tests/test_uv.lua
	./tmg tests/test_uv_fs.lua

SAN_CC ?= $(shell command -v clang >/dev/null 2>&1 && echo clang || echo gcc)
asan:
	$(MAKE) clean
	$(MAKE) \
		CC="$(SAN_CC)" \
		CFLAGS="-std=gnu11 -Wall -Wextra -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" \
		LDFLAGS="-Wl,-E -fsanitize=address,undefined" \
		test

FUZZ_CC ?= clang
FUZZ_CFLAGS ?= -std=gnu11 -Wall -Wextra -O1 -g -fno-omit-frame-pointer
FUZZ_SAN ?= -fsanitize=fuzzer,address,undefined
FUZZ_TIME ?= 30
FUZZ_WORK ?= /tmp/tmg-fuzz
FUZZ_BIN = tests/fuzz_http
FUZZ_SRC = $(SRC)

$(FUZZ_BIN): tests/fuzz_http.c $(FUZZ_SRC) http.h str.h $(LIB_ARCHIVES)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_SAN) -Dmain=tmg_fuzz_main_unused \
		-I. $(CPPFLAGS) tests/fuzz_http.c $(FUZZ_SRC) -o $@ $(LIBS) $(LDFLAGS)

fuzz: $(FUZZ_BIN)
	mkdir -p $(FUZZ_WORK)
	$(FUZZ_BIN) -max_total_time=$(FUZZ_TIME) \
		-artifact_prefix=$(FUZZ_WORK)/ $(FUZZ_WORK) tests/fuzz_corpus

fuzz-smoke: $(FUZZ_BIN)
	mkdir -p $(FUZZ_WORK)
	$(FUZZ_BIN) -runs=2000 -verbosity=0 \
		-artifact_prefix=$(FUZZ_WORK)/ $(FUZZ_WORK) tests/fuzz_corpus

clean:
	rm -f $(OBJ) tmg $(FUZZ_BIN)

distclean: clean
	rm -rf $(UV_DIR)/build $(LLHTTP_DIR)/build $(WSLAY_DIR)/build

.PHONY: all test asan fuzz fuzz-smoke clean distclean
