# Среда сборки плагина под Linux. Плагин жёстко привязан к версии libavcodec в системе,
# поэтому собираю его отдельно под каждое поколение дистрибутивов (см. build-all.sh).
# docker build --build-arg BASE=ubuntu:24.04 -t magercam-linux obs-plugin/linux
ARG BASE=ubuntu:24.04
FROM ${BASE}
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential pkg-config libobs-dev libavcodec-dev libavutil-dev libcairo2-dev git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
# Заголовки libobs 30.0: OBS не грузит плагин, собранный с заголовками новее себя, а с этими плагин
# подойдёт к любому OBS от 30.0, даже если в системе libobs-dev новее, чем сам OBS у пользователя.
RUN git clone -q --depth 1 --branch 30.0.0 --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git /opt/obs-studio \
    && git -C /opt/obs-studio sparse-checkout set libobs
ENV OBS_INC=/opt/obs-studio/libobs
