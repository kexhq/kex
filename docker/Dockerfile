# A container with the Kex toolchain: `kex` on PATH, its stdlib and runtime
# beside it, and the Erlang the BEAM backend needs at run time.
#
#   docker build -f docker/Dockerfile -t kex .
#   docker run --rm -v "$PWD:/work" kex hello.kex
#   docker run --rm -it kex -i            # the REPL
#
# This is the glibc image; docker/Dockerfile.alpine is the musl one, and both
# are supported. They are separate files rather than one parameterised build
# because they are different platforms: the spec suite compares FLOAT OUTPUT
# against golden files, and libm rounds the last bit differently between
# implementations — which is also why CI builds both.

FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git \
        libgmp-dev libpcre2-dev libreadline-dev libboost-context-dev libssl-dev \
        erlang-base erlang-dev erlang-odbc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# KEX_BUILD_DATE is passed in by the release workflow so the binary's stamp is
# the tagged commit's date; a local build just gets today's.
ARG KEX_BUILD_DATE
# Bounded on purpose: an unbounded --parallel runs one compiler per core, and
# this tree has translation units big enough to exhaust a default Docker
# Desktop VM (the build dies with "cannot allocate memory"). CI passes a
# higher number for its bigger runners.
ARG BUILD_JOBS=2
# The build context has no .git (see .dockerignore), so the commit is passed
# in rather than discovered.
ARG KEX_GIT_REVISION
RUN cmake -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
        ${KEX_BUILD_DATE:+-DKEX_BUILD_DATE="$KEX_BUILD_DATE"} \
        ${KEX_GIT_REVISION:+-DKEX_GIT_REVISION="$KEX_GIT_REVISION"} \
    && cmake --build build --parallel "$BUILD_JOBS"

# The image is only worth publishing if the compiler in it works on this
# platform — both backends, since the runtime beams ship with it.
RUN make spec && make spec-beam

RUN cmake --install build --prefix /opt/kex

# Tey as well, because the image is the whole toolchain: every CI system that
# is not GitHub Actions takes an image plus a script, so a container with only
# `kex` in it leaves each of them downloading Tey by hand.
#
# Its OWN prefix, and not /opt/kex, because `make -C tey install` also lays
# down `bin/kex` — the dispatcher that runs whichever toolchain Tey selected —
# which would overwrite the compiler this stage just built. The dispatcher has
# nothing to dispatch here anyway: TEY_KEX in the final stage points Tey at
# the compiler in the image, the only one it will ever have.
RUN make -C tey install PREFIX=/opt/tey KEX=/src/build/kex ERL=erl \
        INSTALL_NONINTERACTIVE=1 \
    && rm -f /opt/tey/bin/kex

FROM debian:bookworm-slim

# Runtime only: the shared libraries the binary links, plus Erlang for `-R`,
# `--compile` and the REPL's BEAM mode.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgmp10 libpcre2-8-0 libreadline8 libboost-context1.74.0 libssl3 \
        erlang-base erlang-odbc \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/kex /opt/kex
COPY --from=build /opt/tey /opt/tey
# /opt/kex FIRST: `kex` must be the compiler, never a dispatcher.
ENV PATH="/opt/kex/bin:/opt/tey/bin:${PATH}"
# Tey manages toolchains by downloading them into a home directory. There is
# nothing to manage in an image that ships exactly one compiler, and a
# container that reaches for the network to find what is already inside it is
# a container that fails on an offline runner.
ENV TEY_KEX="/opt/kex/bin/kex"

# Programs are mounted, not baked: `docker run -v "$PWD:/work" kex main.kex`.
#
# ENTRYPOINT stays `kex`, so `docker run kex main.kex` keeps working. Reach
# Tey with `docker run --entrypoint tey ... test`; CI systems that take an
# `image:` and a `script:` replace the entrypoint themselves and just run
# `tey test`.
WORKDIR /work

ENTRYPOINT ["kex"]
CMD ["--version"]
