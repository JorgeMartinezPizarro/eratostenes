CXX ?= g++
CXXFLAGS_COMMON   := -std=c++20 -Wall -Wextra -pthread
# -static-libgcc/-static-libstdc++ on release and portable (not debug, to
# leave ASan/UBSan's own runtime linking alone): a binary built against a
# newer libstdc++ than the machine it runs on has (e.g. the Docker
# multi-stage build here, where the builder stage's GCC is newer than the
# runtime stage's own libstdc++6, both nominally "bookworm") fails at
# startup with "version `GLIBCXX_3.4.31' not found" -- linking the C++
# runtime into the binary removes that dependency entirely instead of
# needing the two stages' library versions to happen to match.
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

# Un obj/<perfil>/ por perfil de compilacion (release/portable/debug): cada
# uno usa flags distintos, asi que sus .o no pueden compartirse -- si
# vivieran en el mismo directorio, cambiar de perfil enlazaria objetos
# compilados con las flags del perfil anterior sin recompilarlos.
OBJ_DIR_RELEASE  := obj/release
OBJ_DIR_PORTABLE := obj/portable
OBJ_DIR_DEBUG    := obj/debug
OBJ_DIR_PGO      := obj/pgo

OBJS_RELEASE  := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR_RELEASE)/%.o)
OBJS_PORTABLE := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR_PORTABLE)/%.o)
OBJS_DEBUG    := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR_DEBUG)/%.o)

# pgo only ever builds main.cpp (nth_prime isn't a hot loop, see NTH_BIN
# below) and both phases reuse this exact object path -- gcc names each
# .gcda after the .o that produced it, so generate and use must agree on
# that path for -fprofile-use to find the right file under PROF_DIR.
PROF_DIR := $(OBJ_DIR_PGO)/prof
PGO_OBJ  := $(OBJ_DIR_PGO)/main.o

# nth_prime (the .db reader): its own binary, release profile only -- it's
# not a hot loop, so -march=native/portable/debug variants aren't needed.
NTH_BIN := nth_prime
NTH_OBJ := $(OBJ_DIR_RELEASE)/nth_prime.o

OUT_DIR := $(CURDIR)/output
COMPOSE := docker compose -f docker/docker-compose.yml

.PHONY: all portable debug pgo clean fclean re docker run docker-pgo run-pgo \
        test benchmark docker-dev docker-test docker-benchmark

# --- release (default) ---
all: $(BIN) $(NTH_BIN)

$(BIN): $(OBJS_RELEASE)
	$(CXX) $(CXXFLAGS_RELEASE) -o $@ $(OBJS_RELEASE) $(LDLIBS)

$(NTH_BIN): $(NTH_OBJ)
	$(CXX) $(CXXFLAGS_RELEASE) -o $@ $(NTH_OBJ) $(LDLIBS)

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

# --- pgo: profile-guided release build, same flags as release plus two
# full compiles of main.cpp around a training run. Trained without -o
# (count-only) at N=1e9/1e10/1e11 (natural auto -s, crosses the small/
# medium tier thresholds as population grows, see erat_small.hpp) plus
# two forced-width runs so the sparse/bucket tier (process_big,
# segment_sieve.hpp) gets real profile data too -- none of the natural
# passes reach it (auto -s only pushes primes into that tier well past
# 1e12 on typical hardware; see git history for how that crossover was
# found). -o output (.db/text write paths) isn't exercised by any pass
# here, so this profile doesn't inform them.
#   - `1e10 -s 300000`: on THIS dev PC gives a real mix of all three
#     tiers (2687 small / 3817 medium / 3050 sparse) -- kept mainly for
#     the medium+sparse combination, but the exact split is a function of
#     this machine's detected L1 size (small_limit) vs this -s, so a
#     different machine (e.g. the server) may land closer to one tier or
#     the other. That's fine -- it's not required to reproduce this exact
#     split, only to give the sparse tier SOME real samples.
#   - `1e9 -s 16000`: deliberately tiny relative to N (base_limit=31623)
#     so it reliably exercises the sparse tier even if the first run
#     happens to land mostly on one tier on a given machine -- robust
#     across machines because it depends only on N and -s, not on
#     detected cache sizes the way small_limit does.
#     (A natural `1e13` pass, to give the sparse tier real-proportion data
#     instead of just forced-tiny-width coverage, was tried and abandoned
#     -- doesn't scale, an instrumented build is far slower than release
#     and a full 1e13 count-only pass didn't finish in 45+ minutes. See
#     docs/RESEARCH.md.)
# -fprofile-update=prefer-atomic on the instrumented build: plain
# (non-atomic) counters race and undercount under this program's own
# thread pool. -fprofile-correction + -Wno-coverage-mismatch on the final
# build: the CFG built from -flto=auto isn't byte-identical to the
# instrumented run's, which GCC otherwise treats as a hard mismatch
# instead of just missing coverage. Overwrites $(BIN) in place, like
# `portable` does; run `make re` afterwards to get back a plain release
# build.
#
# NOT ADOPTED: measured a real ~2-4% win on the dev PC, but no measurable
# difference on the production server (the actual target hardware, which
# has no perf/cycles:u available to look past wall-clock noise the way
# this project normally would) -- see docs/RESEARCH.md. Kept as opt-in
# infrastructure (not part of default `make`/`make docker`) since it's
# harmless sitting unused, but don't re-run this validation again without
# a new reason to expect a different answer.
pgo: $(NTH_BIN)
	rm -rf $(OBJ_DIR_PGO)
	mkdir -p $(PROF_DIR)
	$(CXX) $(CXXFLAGS_RELEASE) -fprofile-generate -fprofile-update=prefer-atomic \
	    -fprofile-dir=$(PROF_DIR) -c $(SRC_DIR)/main.cpp -o $(PGO_OBJ)
	$(CXX) $(CXXFLAGS_RELEASE) -fprofile-generate -fprofile-update=prefer-atomic \
	    -fprofile-dir=$(PROF_DIR) -o $(BIN) $(PGO_OBJ) $(LDLIBS)
	./$(BIN) 1e9  >/dev/null
	./$(BIN) 1e10 >/dev/null
	./$(BIN) 1e11 >/dev/null
	./$(BIN) 1e10 -s 300000 >/dev/null
	./$(BIN) 1e9  -s 16000  >/dev/null
	$(CXX) $(CXXFLAGS_RELEASE) -fprofile-use -fprofile-correction -Wno-coverage-mismatch \
	    -fprofile-dir=$(PROF_DIR) -c $(SRC_DIR)/main.cpp -o $(PGO_OBJ)
	$(CXX) $(CXXFLAGS_RELEASE) -fprofile-use -fprofile-correction -Wno-coverage-mismatch \
	    -fprofile-dir=$(PROF_DIR) -o $(BIN) $(PGO_OBJ) $(LDLIBS)

$(OBJ_DIR_RELEASE) $(OBJ_DIR_PORTABLE) $(OBJ_DIR_DEBUG) $(OBJ_DIR_PGO):
	mkdir -p $@

clean:
	rm -rf obj

fclean: clean
	rm -f $(BIN) $(BIN)_debug $(NTH_BIN)

re: fclean all

# eratostenes-pgo is NOT built here -- see docker-pgo below for why it's
# a separate, explicit opt-in instead of part of the default set.
docker:
	$(COMPOSE) build eratostenes dev

# Ejecuta el binario dentro de la imagen. El directorio ./output del host
# se monta en /output dentro del contenedor (definido en
# docker/docker-compose.yml): usa -o /output/<fichero> en ARGS para que el
# resultado quede accesible fuera del contenedor.
# `docker compose run` asigna TTY automaticamente cuando la terminal que
# invoca es interactiva (ver -T/--no-TTY en `docker compose run --help`),
# a diferencia de `docker run`, que no lo hace salvo que se le pida -t.
# -e ERATOSTENES_DEBUG_IDLE forwards that var IF set in the host shell
# (see run_parallel_chunks in main.cpp) -- `docker compose run` doesn't
# forward the host environment on its own, has to be told which vars to
# pass through, same as THREADS below for docker-test/docker-benchmark.
# Ejemplo: make run ARGS="1e9 -o /output/primos.txt -t 8"
run:
	mkdir -p $(OUT_DIR)
	$(COMPOSE) run --rm -e ERATOSTENES_DEBUG_IDLE eratostenes $(ARGS)

# PGO image: two-phase profile-guided build (see Makefile's own `pgo`
# target for the flags/training rationale) baked in at `docker build`
# time inside docker/Dockerfile's pgo-builder stage -- for a machine with
# only Docker installed, no gcc/make of its own (the reason this exists:
# some deployment targets, e.g. the production server, are exactly that).
# NOT part of plain `make docker`/`docker-dev` -- explicit opt-in on
# purpose, because unlike the other images this one is tied to the exact
# machine `docker compose build eratostenes-pgo` runs on (-march=native,
# baked in at both the instrumented AND the final compile -- see
# Dockerfile's own warning on pgo-builder) and takes noticeably longer to
# build (the training passes run during the image build itself).
# Not adopted -- tried on the actual production server, no measurable win.
# See the `pgo` target's own comment above and docs/RESEARCH.md for the
# numbers before repeating this investigation.
docker-pgo:
	$(COMPOSE) build eratostenes-pgo

# Same calling convention as `run` above, against the PGO image instead,
# including the ERATOSTENES_DEBUG_IDLE forwarding.
# Ejemplo: make run-pgo ARGS="1e11 -t 8"
run-pgo:
	mkdir -p $(OUT_DIR)
	$(COMPOSE) run --rm -e ERATOSTENES_DEBUG_IDLE eratostenes-pgo $(ARGS)

# Compara pi(N) contra el valor conocido para N=1e8..1e11 (sin -o, modo
# conteo, sin E/S); un .db real en N=1e10 con primos conocidos por posicion via
# nth_prime; y round-trip texto vs .db en N=1e5..1e7, posicion por posicion
# (ver scripts/test.sh). THREADS=N make test para fijar el numero de hilos.
test: $(BIN) $(NTH_BIN)
	./scripts/test.sh

# Dos barridos (ver scripts/benchmark.sh): 1) eratostenes vs primesieve en
# CPU, N=1e10..1e13 -- la tabla de README.md#benchmarks; 2) E/S real,
# construye un .db por cada N en 1e8..1e12 y mide tamano/bits-por-primo/
# throughput/tiempo. THREADS/SEGMENT/REPS/WRITE_PATH/KEEP_DB se pueden
# pasar como variables de entorno (ver el propio script). WRITE_PATH debe
# apuntar al filesystem nativo de Linux (no a un /mnt/c... montado, mucho
# mas lento) -- default: $HOME/eratostenes-io-bench.
benchmark:
	./scripts/benchmark.sh

# --- run the same targets inside Docker (see docker/Dockerfile's `dev`
# stage: gcc + libsqlite3-dev + libzstd-dev + primesieve). Each rebuilds
# eratostenes/nth_prime with the container's own gcc against the
# container's own CPU, so these work the same regardless of what's
# installed/compiled on the host. THREADS/SEGMENT/WRITE_PATH/KEEP_DB/REPS
# are forwarded from the host environment when set, same as running the
# scripts directly (e.g. THREADS=8 make docker-benchmark).
docker-dev:
	$(COMPOSE) build dev

docker-test: docker-dev
	$(COMPOSE) run --rm -e THREADS dev make test

docker-benchmark: docker-dev
	$(COMPOSE) run --rm -e THREADS -e SEGMENT -e REPS -e WRITE_PATH -e KEEP_DB dev make benchmark
