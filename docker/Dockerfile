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

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-0 libzstd1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /src/eratostenes /usr/local/bin/eratostenes
COPY --from=builder /src/nth_prime /usr/local/bin/nth_prime
WORKDIR /output
ENTRYPOINT ["/usr/local/bin/eratostenes"]
CMD ["--help"]
