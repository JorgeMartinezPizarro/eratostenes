# Build multi-stage: compila con g++ y deja una imagen final ligera.
# Release (-march=native) porque se construye y se ejecuta en la misma
# maquina -- si algun dia el build host y el runtime host difieren, volver
# a "make portable" (ver Makefile) para no arriesgar SIGILL en la CPU de
# destino.

FROM gcc:13-bookworm AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-dev libzstd-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN make

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

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-0 libzstd1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /src/eratostenes /usr/local/bin/eratostenes
COPY --from=builder /src/nth_prime /usr/local/bin/nth_prime
WORKDIR /output
ENTRYPOINT ["/usr/local/bin/eratostenes"]
CMD ["--help"]
