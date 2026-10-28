FROM postgres:16-bookworm

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential postgresql-server-dev-16 libkrb5-dev \
    && rm -rf /var/lib/apt/lists/*

COPY src/gsi_fdw /build/gsi_fdw
RUN make -C /build/gsi_fdw && make -C /build/gsi_fdw install
