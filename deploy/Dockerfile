# TheLife on this machine (deploy/README.md). The context is build/deploy, staged by `scripts/prod.sh build`:
# release binaries built on the host (Ubuntu 24.04; vcpkg links everything statically except the C/C++ runtime),
# the built web UI, the LLM config and the migrations. Nothing is compiled here.

# Migrations, baked in so the running deployment does not follow the working tree.
FROM ghcr.io/amacneil/dbmate:2.28 AS migrate
COPY migrations /db/migrations
ENTRYPOINT ["dbmate", "--migrations-dir", "/db/migrations", "--no-dump-schema", "--wait"]
CMD ["up"]

FROM ubuntu:24.04 AS backend
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates tzdata curl \
 && rm -rf /var/lib/apt/lists/*
COPY bin/ /opt/thelife/bin/
COPY web/ /opt/thelife/web/
COPY config/ /opt/thelife/config/
COPY VERSION /opt/thelife/VERSION
ENV PATH=/opt/thelife/bin:$PATH \
    THELIFE_WEB_ROOT=/opt/thelife/web \
    THELIFE_LLM_CONFIG=/opt/thelife/config/llm.json \
    THELIFE_UPLOAD_DIR=/tmp/uploads
WORKDIR /opt/thelife
ENTRYPOINT ["thelife-backend"]
