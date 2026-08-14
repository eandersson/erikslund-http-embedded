
ARG TOOLCHAIN_IMAGE=erikslund-http-build

FROM ${TOOLCHAIN_IMAGE} AS builder

COPY . /src

RUN cmake -S /src -B /build \
        -DCMAKE_BUILD_TYPE=Release \
        -DERIKSLUND_HTTP_BUILD_TESTS=OFF \
        -DERIKSLUND_HTTP_BUILD_EXAMPLES=ON \
        -DERIKSLUND_HTTP_INSTALL=OFF \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    && cmake --build /build -j"$(nproc)" --target status_server

FROM debian:trixie-slim@sha256:3a39a0592364683e6bab97937b72cad5a8fa6dcbbee90edb3bb48c7f8e94f258 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3t64 \
        curl \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --uid 1000 --user-group --no-create-home --shell /usr/sbin/nologin erikslund \
    && mkdir -p /tls /control \
    && chown 1000:1000 /tls /control

COPY --from=builder /build/examples/status_server /usr/local/bin/status-server

USER 1000:1000
WORKDIR /tmp

EXPOSE 7777 7778

HEALTHCHECK --interval=2s --timeout=2s --start-period=2s --retries=15 \
    CMD ["curl", "--fail", "--silent", "--show-error", "--output", "/dev/null", \
         "http://127.0.0.1:7777/healthz"]

ENTRYPOINT ["/usr/local/bin/status-server"]

FROM debian:trixie-slim@sha256:3a39a0592364683e6bab97937b72cad5a8fa6dcbbee90edb3bb48c7f8e94f258 AS tester

RUN apt-get update && apt-get install -y --no-install-recommends \
        curl \
        openssl \
        jq \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --uid 1000 --user-group --no-create-home --shell /usr/sbin/nologin erikslund \
    && mkdir -p /tls /control \
    && chown 1000:1000 /tls /control

COPY e2e/scripts/generate-test-certificate.sh /usr/local/bin/generate-test-certificate.sh
COPY e2e/scripts/checks.sh /usr/local/bin/e2e-checks.sh
RUN chmod +x /usr/local/bin/generate-test-certificate.sh /usr/local/bin/e2e-checks.sh

USER 1000:1000
WORKDIR /tmp

ENTRYPOINT ["/usr/local/bin/e2e-checks.sh"]
