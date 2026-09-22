# Build multi-stage: compila con g++ y deja una imagen final ligera.
# Se usa "make portable" (sin -march=native) porque la imagen puede
# ejecutarse en una CPU distinta a la que la construyo.

FROM gcc:13-bookworm AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-dev libzstd-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN make portable nth_prime

# dev: full toolchain image for `make test`/`benchmark` inside Docker (see
# the `dev` service in docker-compose.yml). Those targets
# compile from source themselves (release build, -march=native against the
# container's own CPU) rather than just running prebuilt binaries, so this
# stage keeps gcc/make instead of shipping only the runtime image below.
# primesieve is added on top for benchmark.sh's comparison column.
FROM builder AS dev
RUN apt-get update && apt-get install -y --no-install-recommends \
        primesieve \
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
