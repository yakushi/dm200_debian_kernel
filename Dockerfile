FROM ubuntu:14.04 AS build-kernel

RUN apt-get update && apt-get install -y \
    bc \
    build-essential \
    gcc-arm-linux-gnueabihf \
    lzop \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*

COPY scripts/mkkrnlimg.c /tmp/
RUN gcc -o /usr/local/bin/mkkrnlimg /tmp/mkkrnlimg.c

WORKDIR /linux

ENV ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-

CMD make defconfig dm200_defconfig && \
    make dep && \
    make zImage && \
    mkkrnlimg arch/arm/boot/zImage kernel.img