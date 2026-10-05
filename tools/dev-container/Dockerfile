# Everything CI has, in one image: the pinned NDK (r21e, as build.yml), the test packages, mksh/shellcheck for
# `make lint`, and the Python of the protocol tests with tests/requirements.txt.  For PCs without Linux (Windows: Docker
# Desktop) and for running the whole suite the way CI does.  Built from the repository root:
#   docker build -t hassmic-dev -f tools/dev-container/Dockerfile .
# then tools/dev-container/run.sh '<command>' (see there).
FROM python:3.13-bookworm
RUN apt-get update -qq && apt-get install -y -qq build-essential pkg-config libopus-dev libcurl4 netcat-openbsd \
        ffmpeg sbc-tools mksh procps unzip curl git bc shellcheck > /dev/null && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL -o /tmp/ndk.zip https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip && \
    echo "ad7ce5467e18d40050dc51b8e7affc3e635c85bd8c59be62de32352328ed467e  /tmp/ndk.zip" | sha256sum -c - && \
    unzip -q /tmp/ndk.zip -d /opt && rm /tmp/ndk.zip
COPY tests/requirements.txt /tmp/
# at /src/.venv, where the Makefile and the tests look: run.sh mounts a named volume there, which Docker fills from
# this layer the first time, so the checkout's own .venv (if any, e.g. a Windows one) stays out of it
RUN python -m venv /src/.venv && /src/.venv/bin/pip install -q -r /tmp/requirements.txt
ENV NDK=/opt/android-ndk-r21e
RUN git config --global --add safe.directory '*'
WORKDIR /src
