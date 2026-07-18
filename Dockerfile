FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libnl-3-dev \
        libnl-route-3-dev \
        libpcap-dev \
        libprotobuf-dev \
        libqt5svg5-dev \
        ninja-build \
        pkg-config \
        protobuf-compiler \
        qtbase5-dev

WORKDIR /workspace

CMD ["bash"]
