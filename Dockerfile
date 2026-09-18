# Build multi-stage: compila con g++ y deja una imagen final ligera.
# Se usa "make portable" (sin -march=native) porque la imagen puede
# ejecutarse en una CPU distinta a la que la construyo.

FROM gcc:13-bookworm AS builder
WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN make portable

FROM debian:bookworm-slim
COPY --from=builder /src/eratostenes /usr/local/bin/eratostenes
ENV TERM=xterm-256color 
WORKDIR /output
ENTRYPOINT ["/usr/local/bin/eratostenes"]
CMD ["--help"]
