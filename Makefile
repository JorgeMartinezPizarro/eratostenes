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

OBJS_RELEASE  := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR_RELEASE)/%.o)
OBJS_PORTABLE := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR_PORTABLE)/%.o)
OBJS_DEBUG    := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR_DEBUG)/%.o)

# nth_prime (the .db reader): its own binary, release profile only -- it's
# not a hot loop, so -march=native/portable/debug variants aren't needed.
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

# Binario de prueba con flags de compilacion extra para un A/B contra el
# binario normal: los mandos ERA_* de sparse_tier.hpp (medidos en
# docs/RESEARCH.md) o cualquier flag del compilador. Se recompila siempre:
#   make variant DEFS=-DERA_BIG_PF=8
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

# Ejecuta el binario dentro de la imagen. El directorio ./output del host
# se monta en /output dentro del contenedor (definido en
# docker/docker-compose.yml): usa -o /output/<fichero> en ARGS para que el
# resultado quede accesible fuera del contenedor.
# `docker compose run` asigna TTY automaticamente cuando la terminal que
# invoca es interactiva (ver -T/--no-TTY en `docker compose run --help`),
# a diferencia de `docker run`, que no lo hace salvo que se le pida -t.
# Toda la configuracion va en ARGS como flags (ver --help), tambien las de
# benchmark: --start, --debug-idle y --tune clave=valor.
# Ejemplo: make run ARGS="1e9 -o /output/primos.txt -t 8"
# Ejemplo: make run ARGS="1e15 --start 990e12 --debug-idle"
run:
	mkdir -p $(OUT_DIR)
	$(COMPOSE) run --rm eratostenes $(ARGS)

# Consulta un .db con nth_prime dentro de la imagen (su ENTRYPOINT es
# eratostenes, de ahi el --entrypoint). Mismo montaje que `run`: el .db
# tiene que estar en ./output del host y se nombra como /output/<fichero>.
# Ejemplos: make nth-prime ARGS="/output/primos.db 1000000"
#           make nth-prime ARGS="/output/primos.db --count"
nth-prime:
	mkdir -p $(OUT_DIR)
	$(COMPOSE) run --rm --entrypoint nth_prime eratostenes $(ARGS)

# Compara pi(N) contra el valor conocido para N=1e8..1e11 (sin -o, modo
# conteo, sin E/S); un .db real en N=1e10 con primos conocidos por posicion via
# nth_prime; y round-trip texto vs .db en N=1e5..1e7, posicion por posicion
# (ver scripts/test.sh). THREADS=N make test para fijar el numero de hilos.
test: $(BIN) $(NTH_BIN)
	./scripts/test.sh

# eratostenes vs primesieve en modo conteo, N=1e10..1e13 (ver
# scripts/benchmark.sh) -- la tabla de docs/BENCHMARK.md y del README.
# THREADS/SEGMENT/REPS como variables de entorno.
benchmark:
	./scripts/benchmark.sh

# E/S real (ver scripts/benchmark_io.sh): construye un .db por cada N en
# 1e8..1e12 y mide tamano/bits-por-primo/throughput/tiempo -- la tabla de
# README.md#database. THREADS/SEGMENT/WRITE_PATH/KEEP_DB como variables de
# entorno. WRITE_PATH debe apuntar al filesystem nativo de Linux (no a un
# /mnt/c... montado, mucho mas lento) -- default: $HOME/eratostenes-io-bench.
benchmark-io:
	bash scripts/benchmark_io.sh

# Cima del rango frente a primesieve (ver scripts/benchmark_tails.sh): los
# ultimos 1e11 numeros por debajo de 1e14..1e18 (0.1%..0.00001%), mismo
# numero de hilos en los dos, recuentos cruzados entre si.
# THREADS/REPS/NS/WIDTH como variables de entorno.
# Ejemplo: REPS=2 THREADS=2 make benchmark-tails
benchmark-tails: $(BIN)
	bash scripts/benchmark_tails.sh

# Diagnostico corto para una maquina nueva o vieja (ver scripts/benchmark_mini.sh):
# cachés según sysfs, lo que eligió el automático y un barrido de segmento,
# corte sparse y prefetch sobre la ultima 1e10 bajo 1e13, contra primesieve.
# N/WIDTH/THREADS/SEGMENTS como variables de entorno.
benchmark-mini: $(BIN)
	bash scripts/benchmark_mini.sh

# A/B intercalado de dos ejecuciones de eratostenes sobre la cola de uno o
# varios N (ver scripts/benchmark_ab.sh): cada lado es un binario y unas
# opciones; perf stat cycles:u/instructions:u donde hay perf, si no tiempo.
#   B="--tune sparse=1/2" make benchmark-ab
#   BIN_A=/tmp/era_A/eratostenes NS="1e15 1e17" make benchmark-ab
# BIN_A/BIN_B/A/B/NS/WIDTH/THREADS/REPS/EVENTS/PERF como variables de entorno.
benchmark-ab: $(BIN)
	bash scripts/benchmark_ab.sh

# --- run the same targets inside Docker (see docker/Dockerfile's `dev`
# stage: gcc + libsqlite3-dev + libzstd-dev + primesieve). Each rebuilds
# eratostenes/nth_prime with the container's own gcc against the
# container's own CPU, so these work the same regardless of what's
# installed/compiled on the host. THREADS/SEGMENT/WRITE_PATH/KEEP_DB/REPS
# are forwarded from the host environment when set, same as running the
# scripts directly (e.g. THREADS=8 make docker-benchmark); N/WIDTH/SEGMENTS
# likewise for docker-benchmark-mini.
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
