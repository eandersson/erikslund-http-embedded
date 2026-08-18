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

ADD --checksum=sha256:3436de5103825f81132500463dbcd8aab1d18a97205a6e046b13f365f338575d \
    https://codeload.github.com/stephenberry/glaze/tar.gz/07e5ef2e2d4c53fdfd8603a61be37053b5a27bc5 \
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
