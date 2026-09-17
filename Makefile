CXX ?= g++
CXXFLAGS_COMMON := -std=c++20 -Wall -Wextra -pthread
CXXFLAGS_RELEASE := $(CXXFLAGS_COMMON) -O3 -march=native -flto
CXXFLAGS_PORTABLE := $(CXXFLAGS_COMMON) -O3
CXXFLAGS_DEBUG := $(CXXFLAGS_COMMON) -O0 -g -fsanitize=address,undefined

SRC := src/main.cpp
BIN := eratostenes

IMAGE := eratostenes:latest
OUT_DIR := $(CURDIR)/output

.PHONY: all portable debug clean docker run test

all: $(BIN)

$(BIN): $(SRC) src/*.hpp
	$(CXX) $(CXXFLAGS_RELEASE) -o $(BIN) $(SRC)

portable: $(SRC) src/*.hpp
	$(CXX) $(CXXFLAGS_PORTABLE) -o $(BIN) $(SRC)

debug: $(SRC) src/*.hpp
	$(CXX) $(CXXFLAGS_DEBUG) -o $(BIN)_debug $(SRC)

clean:
	rm -f $(BIN) $(BIN)_debug

docker:
	docker build -t $(IMAGE) .

# Ejecuta el binario dentro de la imagen. El directorio ./output del host
# se monta en /output dentro del contenedor: usa -o /output/<fichero> para
# que el resultado quede accesible fuera del contenedor.
# Ejemplo: make run ARGS="--limit 1e9 -o /output/primos.txt -t 8"
run:
	mkdir -p $(OUT_DIR)
	docker run --rm -v $(OUT_DIR):/output $(IMAGE) $(ARGS)

# Compara pi(N) contra el valor conocido para N=1e8..1e11 (--count-only,
# sin E/S). THREADS=N make test para fijar el numero de hilos.
test: $(BIN)
	./test.sh
