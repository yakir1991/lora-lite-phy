FROM mcr.microsoft.com/devcontainers/base:ubuntu

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    git \
    libliquid-dev \
    valgrind \
    ccache \
    clang-format \
    gnuradio gnuradio-dev \
    && rm -rf /var/lib/apt/lists/*

