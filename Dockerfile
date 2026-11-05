# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) Opinsys Oy 2026

FROM debian:trixie-slim@sha256:f6b08b09a05725f9431502d3b5714f5c4576e82c1a84fa1debe0c24a7e0c3ec4

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get -o APT::Retries=3 update \
    && apt-get -o APT::Retries=3 install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        clang-format \
        cmake \
        git \
        libabsl-dev \
        libblkid-dev \
        libfmt-dev \
        libftxui-dev \
        libmount-dev \
        libspdlog-dev \
        libssl-dev \
        libudev-dev \
        libyaml-cpp-dev \
        ninja-build \
        nlohmann-json3-dev \
        pkg-config \
        shellcheck \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
