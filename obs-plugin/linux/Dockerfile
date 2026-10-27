# Среда сборки плагина под Linux. Плагин жёстко привязан к версии libavcodec в системе,
# поэтому собираю его отдельно под каждое поколение дистрибутивов (см. build-all.sh).
# docker build --build-arg BASE=ubuntu:24.04 -t magercam-linux obs-plugin/linux
ARG BASE=ubuntu:24.04
FROM ${BASE}
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential pkg-config libobs-dev libavcodec-dev libavutil-dev libcairo2-dev \
    && rm -rf /var/lib/apt/lists/*
