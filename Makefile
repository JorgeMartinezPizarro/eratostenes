CXX ?= g++
CXXFLAGS_COMMON   := -std=c++20 -Wall -Wextra -pthread
CXXFLAGS_RELEASE  := $(CXXFLAGS_COMMON) -O3 -march=native -flto
CXXFLAGS_PORTABLE := $(CXXFLAGS_COMMON) -O3
CXXFLAGS_DEBUG    := $(CXXFLAGS_COMMON) -O0 -g -fsanitize=address,undefined

BIN     := eratostenes
SRC_DIR := src
SRCS    := $(wildcard $(SRC_DIR)/*.cpp)
HEADERS := $(wildcard $(SRC_DIR)/*.hpp)

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

OUT_DIR := $(CURDIR)/output
COMPOSE := docker compose -f docker/docker-compose.yml

.PHONY: all portable debug clean fclean re docker run test

# --- release (default) ---
all: $(BIN)

$(BIN): $(OBJS_RELEASE)
	$(CXX) $(CXXFLAGS_RELEASE) -o $@ $(OBJS_RELEASE)

$(OBJ_DIR_RELEASE)/%.o: $(SRC_DIR)/%.cpp $(HEADERS) | $(OBJ_DIR_RELEASE)
	$(CXX) $(CXXFLAGS_RELEASE) -c $< -o $@

# --- portable: no -march=native, for a binary you'll copy to another machine ---
portable: $(OBJS_PORTABLE)
	$(CXX) $(CXXFLAGS_PORTABLE) -o $(BIN) $(OBJS_PORTABLE)

$(OBJ_DIR_PORTABLE)/%.o: $(SRC_DIR)/%.cpp $(HEADERS) | $(OBJ_DIR_PORTABLE)
	$(CXX) $(CXXFLAGS_PORTABLE) -c $< -o $@

# --- debug: ASan/UBSan ---
debug: $(OBJS_DEBUG)
	$(CXX) $(CXXFLAGS_DEBUG) -o $(BIN)_debug $(OBJS_DEBUG)

$(OBJ_DIR_DEBUG)/%.o: $(SRC_DIR)/%.cpp $(HEADERS) | $(OBJ_DIR_DEBUG)
	$(CXX) $(CXXFLAGS_DEBUG) -c $< -o $@

$(OBJ_DIR_RELEASE) $(OBJ_DIR_PORTABLE) $(OBJ_DIR_DEBUG):
	mkdir -p $@

clean:
	rm -rf obj

fclean: clean
	rm -f $(BIN) $(BIN)_debug

re: fclean all

docker:
	$(COMPOSE) build

# Ejecuta el binario dentro de la imagen. El directorio ./output del host
# se monta en /output dentro del contenedor (definido en
# docker/docker-compose.yml): usa -o /output/<fichero> en ARGS para que el
# resultado quede accesible fuera del contenedor.
# `docker compose run` asigna TTY automaticamente cuando la terminal que
# invoca es interactiva (ver -T/--no-TTY en `docker compose run --help`),
# a diferencia de `docker run`, que no lo hace salvo que se le pida -t.
# Ejemplo: make run ARGS="--limit 1e9 -o /output/primos.txt -t 8"
run:
	mkdir -p $(OUT_DIR)
	$(COMPOSE) run --rm eratostenes $(ARGS)

# Compara pi(N) contra el valor conocido para N=1e8..1e11 (--count-only,
# sin E/S). THREADS=N make test para fijar el numero de hilos.
test: $(BIN)
	./scripts/test.sh
