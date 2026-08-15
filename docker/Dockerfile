# syntax=docker/dockerfile:1.7@sha256:a57df69d0ea827fb7266491f2813635de6f17269be881f696fbfdf2d83dda33e

# One pinned image backs every build and analysis gate.
FROM gcc:16.2.0-trixie@sha256:b69972d6afe6d5f6c7f91ae30027caf8a50549ee25b63031071383d00941bf2f

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get upgrade -y --no-install-recommends \
    && apt-get install -y --no-install-recommends \
        cmake \
        make \
        libssl-dev \
        zlib1g-dev \
        doctest-dev \
        clang-tidy \
        cppcheck \
        pkg-config \
        ca-certificates \
    && apt-get purge -y abigail-tools python3-git \
    && rm -rf /var/lib/apt/lists/*

ADD --checksum=sha256:8da58a7b58e5373adfd0b1940b9db8e1506cf729491440f997be585a3af08335 \
    https://codeload.github.com/stephenberry/glaze/tar.gz/be4481f4b106fc82f0b7bc85f6a92202f8c0dd59 \
    /tmp/glaze.tar.gz
RUN mkdir /tmp/glaze \
    && tar -xzf /tmp/glaze.tar.gz --strip-components=1 -C /tmp/glaze \
    && cp -r /tmp/glaze/include/glaze /usr/local/include/glaze \
    && rm -rf /tmp/glaze /tmp/glaze.tar.gz

COPY build-entrypoint.sh /usr/local/bin/build-entrypoint.sh
COPY clang-tidy.sh /usr/local/bin/clang-tidy.sh
COPY cppcheck.sh /usr/local/bin/cppcheck.sh
COPY reflection-build.sh /usr/local/bin/reflection-build.sh
RUN chmod +x /usr/local/bin/build-entrypoint.sh /usr/local/bin/clang-tidy.sh \
        /usr/local/bin/cppcheck.sh /usr/local/bin/reflection-build.sh

WORKDIR /build
ENTRYPOINT ["/usr/local/bin/build-entrypoint.sh"]
