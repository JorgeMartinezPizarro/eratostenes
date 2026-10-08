CXX ?= g++
CXXFLAGS_COMMON   := -std=c++20 -Wall -Wextra -pthread
# -static-libgcc/-static-libstdc++ on release and portable (not debug, which
# links the sanitizers' runtime): the binary then runs where the libstdc++ is
# older than the compiler's (e.g. the Docker runtime stage).
CXXFLAGS_RELEASE  := $(CXXFLAGS_COMMON) -O3 -march=native -flto=auto -static-libgcc -static-libstdc++
CXXFLAGS_PORTABLE := $(CXXFLAGS_COMMON) -O3 -static-libgcc -static-libstdc++
CXXFLAGS_DEBUG    := $(CXXFLAGS_COMMON) -O0 -g -fsanitize=address,undefined

BIN     := eratostenes
SRC_DIR := src
# nth_prime.cpp has its own main() (the .db reader) -- built as its own
# binary below, not linked into $(BIN).
SRCS    := $(filter-out $(SRC_DIR)/nth_prime.cpp,$(wildcard $(SRC_DIR)/*.cpp))
HEADERS := $(wildcard $(SRC_DIR)/*.hpp)

# Both eratostenes (.db output mode) and nth_prime (.db reader) link these.
LDLIBS := -lsqlite3 -lzstd

# One obj/<profile>/ per build profile: their flags differ, so their .o
# files can't be shared.
OBJ_DIR_RELEASE  := obj/release
OBJ_DIR_PORTABLE := obj/portable
OBJ_DIR_DEBUG    := obj/debug

OBJS_RELEASE  := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR_RELEASE)/%.o)
OBJS_PORTABLE := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR_PORTABLE)/%.o)
OBJS_DEBUG    := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR_DEBUG)/%.o)

# nth_prime (the .db reader): its own binary, release profile only.
NTH_BIN := nth_prime
NTH_OBJ := $(OBJ_DIR_RELEASE)/nth_prime.o

OUT_DIR := $(CURDIR)/output
COMPOSE := docker compose -f docker/docker-compose.yml
# The dev image has no .git: hand the benchmark scripts the host's commit
# (scripts/machine_info.sh prints it above every table).
GIT_DESC := $(shell git describe --always --dirty 2>/dev/null || echo "?")
COMPOSE_ENV := -e ERATOSTENES_COMMIT=$(GIT_DESC)

.PHONY: all portable debug clean fclean re docker run nth-prime variant \
        test benchmark benchmark-io benchmark-tails benchmark-mini benchmark-ab docker-dev docker-test docker-benchmark \
        docker-benchmark-io docker-benchmark-tails docker-benchmark-mini docker-benchmark-ab

# --- release (default) ---
all: $(BIN) $(NTH_BIN)

$(BIN): $(OBJS_RELEASE)
	$(CXX) $(CXXFLAGS_RELEASE) -o $@ $(OBJS_RELEASE) $(LDLIBS)

$(NTH_BIN): $(NTH_OBJ)
	$(CXX) $(CXXFLAGS_RELEASE) -o $@ $(NTH_OBJ) $(LDLIBS)

# A test binary with extra compile flags, for an A/B against the normal one
# (sparse_tier.hpp's ERA_* knobs or any compiler flag). Always rebuilt:
#   make variant DEFS=-DERA_ACT_IDX=0
#   BIN_B=./eratostenes_variant make benchmark-ab
BIN_VARIANT := eratostenes_variant
$(BIN_VARIANT): $(SRC_DIR)/main.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS_RELEASE) $(DEFS) -o $@ $(SRC_DIR)/main.cpp $(LDLIBS)
variant:
	$(MAKE) -B $(BIN_VARIANT) DEFS="$(DEFS)"

$(OBJ_DIR_RELEASE)/%.o: $(SRC_DIR)/%.cpp $(HEADERS) | $(OBJ_DIR_RELEASE)
	$(CXX) $(CXXFLAGS_RELEASE) -c $< -o $@

# --- portable: no -march=native, for a binary you'll copy to another machine ---
portable: $(OBJS_PORTABLE)
	$(CXX) $(CXXFLAGS_PORTABLE) -o $(BIN) $(OBJS_PORTABLE) $(LDLIBS)

$(OBJ_DIR_PORTABLE)/%.o: $(SRC_DIR)/%.cpp $(HEADERS) | $(OBJ_DIR_PORTABLE)
	$(CXX) $(CXXFLAGS_PORTABLE) -c $< -o $@

# --- debug: ASan/UBSan ---
debug: $(OBJS_DEBUG)
	$(CXX) $(CXXFLAGS_DEBUG) -o $(BIN)_debug $(OBJS_DEBUG) $(LDLIBS)

$(OBJ_DIR_DEBUG)/%.o: $(SRC_DIR)/%.cpp $(HEADERS) | $(OBJ_DIR_DEBUG)
	$(CXX) $(CXXFLAGS_DEBUG) -c $< -o $@

$(OBJ_DIR_RELEASE) $(OBJ_DIR_PORTABLE) $(OBJ_DIR_DEBUG):
	mkdir -p $@

clean:
	rm -f $(BIN_VARIANT); rm -rf obj

fclean: clean
	rm -f $(BIN) $(BIN)_debug $(NTH_BIN)

re: fclean all

docker:
	$(COMPOSE) build eratostenes dev

# Runs the binary inside the image. The host's ./output is mounted at
# /output (docker/docker-compose.yml): use -o /output/<file> in ARGS to get
# the result out of the container. Every option goes in ARGS (see --help).
# Example: make run ARGS="1e9 -o /output/primes.txt -t 8"
# Example: make run ARGS="1e15 --start 990e12 --debug-idle"
run:
	mkdir -p $(OUT_DIR)
	$(COMPOSE) run --rm eratostenes $(ARGS)

# Queries a .db with nth_prime inside the image (same mount as `run`).
# Examples: make nth-prime ARGS="/output/primes.db 1000000"
#           make nth-prime ARGS="/output/primes.db --count"
nth-prime:
	mkdir -p $(OUT_DIR)
	$(COMPOSE) run --rm --entrypoint nth_prime eratostenes $(ARGS)

# Correctness against primecount: counts, tails, .db and text output,
# nth_prime queries, argument errors (scripts/test.sh). THREADS=N fixes
# the thread count.
test: $(BIN) $(NTH_BIN)
	./scripts/test.sh

# eratostenes vs primesieve, count mode (scripts/benchmark.sh): the tables
# of docs/BENCHMARK.md and the README. THREADS/SEGMENT/REPS from the
# environment.
benchmark:
	./scripts/benchmark.sh

# Real I/O (scripts/benchmark_io.sh): a .db per N in 1e8..1e12, size,
# bits/prime, throughput, time. THREADS/SEGMENT/WRITE_PATH/KEEP_DB from the
# environment; WRITE_PATH on a native Linux filesystem (default:
# $HOME/eratostenes-io-bench).
benchmark-io:
	bash scripts/benchmark_io.sh

# Top of the range vs primesieve (scripts/benchmark_tails.sh): the last 1e11
# numbers below 1e14..1e18, same threads for both, counts cross-checked.
# THREADS/REPS/NS/WIDTH from the environment.
# Example: REPS=2 THREADS=2 make benchmark-tails
benchmark-tails: $(BIN)
	bash scripts/benchmark_tails.sh

# Short diagnosis of a machine (scripts/benchmark_mini.sh): caches per
# sysfs, the automatic choices, and a sweep of segment, sparse cutoff and
# prefetch over the last 1e10 below 1e13, against primesieve.
# N/WIDTH/THREADS/SEGMENTS from the environment.
benchmark-mini: $(BIN)
	bash scripts/benchmark_mini.sh

# Interleaved A/B of two eratostenes runs on the tail of one or more N
# (scripts/benchmark_ab.sh): each side a binary and options; perf stat
# cycles:u/instructions:u where perf is available, wall time otherwise.
#   B="--tune sparse=1/2" make benchmark-ab
#   BIN_A=/tmp/era_A/eratostenes NS="1e15 1e17" make benchmark-ab
# BIN_A/BIN_B/A/B/NS/WIDTH/THREADS/REPS/EVENTS/PERF from the environment.
benchmark-ab: $(BIN)
	bash scripts/benchmark_ab.sh

# --- the same targets inside Docker (docker/Dockerfile's `dev` stage), built
# with the container's gcc for its CPU. The environment variables above are
# forwarded when set (e.g. THREADS=8 make docker-benchmark).
docker-dev:
	$(COMPOSE) build dev

docker-test: docker-dev
	$(COMPOSE) run --rm -e THREADS dev make test

docker-benchmark: docker-dev
	$(COMPOSE) run --rm -e THREADS -e SEGMENT -e REPS $(COMPOSE_ENV) dev make benchmark

docker-benchmark-io: docker-dev
	$(COMPOSE) run --rm -e THREADS -e SEGMENT -e WRITE_PATH -e KEEP_DB $(COMPOSE_ENV) dev make benchmark-io

docker-benchmark-tails: docker-dev
	$(COMPOSE) run --rm -e THREADS -e REPS -e NS -e WIDTH -e SEGMENT $(COMPOSE_ENV) dev make benchmark-tails

docker-benchmark-mini: docker-dev
	$(COMPOSE) run --rm -e THREADS -e N -e WIDTH -e SEGMENTS $(COMPOSE_ENV) dev make benchmark-mini

docker-benchmark-ab: docker-dev
	$(COMPOSE) run --rm -e THREADS -e N -e NS -e WIDTH -e REPS -e A -e B -e BIN_A -e BIN_B -e EVENTS -e PERF $(COMPOSE_ENV) dev make benchmark-ab
