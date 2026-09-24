# Build multi-stage: compila con g++ y deja una imagen final ligera.
# Release (-march=native) porque se construye y se ejecuta en la misma
# maquina -- si algun dia el build host y el runtime host difieren, volver
# a "make portable" (ver Makefile) para no arriesgar SIGILL en la CPU de
# destino. La misma condicion se aplica, mas estricta todavia, a la etapa
# pgo-builder de mas abajo: ahi tanto el entrenamiento como la compilacion
# final quedan atados al -march=native del host donde corre `docker
# build`, asi que esa imagen SOLO vale si se construye en la maquina
# donde va a correr (p.ej. el propio servidor, ejecutando `docker build`
# ahi mismo) -- nunca construida en un sitio y copiada/desplegada en otro.
FROM gcc:13-bookworm AS builder-base
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-dev libzstd-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src ./src

FROM builder-base AS builder
RUN make

# pgo-builder: mismo toolchain y fuente que builder, pero corre el build
# de dos fases guiado por perfil (`make pgo` -- ver el propio target en
# Makefile para el razonamiento de flags/entrenamiento) en vez de un
# `make` normal. Esta es la razon de ser de esta imagen: el servidor de
# destino no tiene gcc/make propios, solo Docker -- asi que el ciclo
# instrumentar/entrenar/recompilar tiene que pasar aqui, una vez, en
# tiempo de `docker build`, y quedar ya horneado en la imagen de abajo en
# vez de repetirse en el servidor cada vez.
FROM builder-base AS pgo-builder
RUN make pgo

# dev: full toolchain image for `make test`/`benchmark` inside Docker (see
# the `dev` service in docker-compose.yml). Those targets
# compile from source themselves (release build, -march=native against the
# container's own CPU) rather than just running prebuilt binaries, so this
# stage keeps gcc/make instead of shipping only the runtime image below.
# primesieve is added on top for benchmark.sh's comparison column;
# primecount-bin is what scripts/test.sh checks pi(N)/nth-prime against
# instead of hardcoded constants -- required for `make test` to even start.
FROM builder AS dev
RUN apt-get update && apt-get install -y --no-install-recommends \
        primesieve primecount-bin \
    && rm -rf /var/lib/apt/lists/*
COPY scripts ./scripts
CMD ["make", "test"]

# Named explicitly (and pinned via `target:` in docker-compose.yml) rather
# than left as "whichever stage comes last" -- eratostenes-pgo below is
# defined after this one, and Docker's own default (build the last stage
# when no --target is given) would otherwise silently switch what a plain
# `docker build .` produces the moment that stage was added.
FROM debian:bookworm-slim AS eratostenes
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-0 libzstd1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /src/eratostenes /usr/local/bin/eratostenes
COPY --from=builder /src/nth_prime /usr/local/bin/nth_prime
WORKDIR /output
ENTRYPOINT ["/usr/local/bin/eratostenes"]
CMD ["--help"]

# Same runtime image as eratostenes above, just sourced from pgo-builder
# instead of builder -- the PGO binary, ready to run on a machine with
# only Docker installed. See pgo-builder's own comment for why this image
# must be built ON the machine it will run on.
FROM debian:bookworm-slim AS eratostenes-pgo
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-0 libzstd1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=pgo-builder /src/eratostenes /usr/local/bin/eratostenes
COPY --from=pgo-builder /src/nth_prime /usr/local/bin/nth_prime
WORKDIR /output
ENTRYPOINT ["/usr/local/bin/eratostenes"]
CMD ["--help"]
