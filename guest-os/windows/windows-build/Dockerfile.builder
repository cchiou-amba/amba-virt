FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && \
    apt-get install -y -qq \
        wimtools \
        ntfs-3g \
        dosfstools \
        parted \
        mtools \
        qemu-utils \
        p7zip-full \
        genisoimage \
        xorriso \
        bc \
        curl \
        libhivex-bin \
        python3 \
        python3-hivex \
        && rm -rf /var/lib/apt/lists/*

WORKDIR /work
