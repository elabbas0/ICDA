FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        dosfstools \
        gcc \
        gdisk \
        grub-efi-amd64-bin \
        grub-pc-bin \
        grub-common \
        make \
        mtools \
        nasm \
        ovmf \
        qemu-system-x86 \
        xorriso \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["make"]
