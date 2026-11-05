# `dev` is the toolchain and locked dependencies; `runtime` is the proxy alone.
FROM ubuntu:24.04 AS dev
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential ca-certificates git python3 python3-venv \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY requirements-dev.txt conanfile.txt conan.lock ./
COPY conan/ conan/
COPY scripts/ scripts/
RUN scripts/bootstrap.sh release

FROM dev AS build
COPY . .
RUN .venv/bin/cmake --preset release \
    && .venv/bin/cmake --build --preset release \
    && .venv/bin/ctest --preset release

FROM ubuntu:24.04 AS runtime
RUN useradd --system --no-create-home fluxgate
COPY --from=build /src/build/release/fluxgate /usr/local/bin/fluxgate
COPY config/docker.yaml /etc/fluxgate/proxy.yaml
USER fluxgate
EXPOSE 8080
ENTRYPOINT ["fluxgate", "--config", "/etc/fluxgate/proxy.yaml"]
