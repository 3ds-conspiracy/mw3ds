# Builds mw3ds.cia from your own Morrowind install. Run tools/docker/build.ps1 (it passes the install in as
# the "morrowind" build context, from MW3DS_MORROWIND_DIR in .env). See tools/docker/README.md.
# makerom, built from source: the release binaries need a newer glibc than the devkitPro image has
FROM debian:bookworm AS makerom
RUN apt-get update && apt-get install -y --no-install-recommends build-essential git ca-certificates     && rm -rf /var/lib/apt/lists/*
ARG MAKEROM_VERSION=v0.19.0
RUN git clone --depth 1 --branch makerom-${MAKEROM_VERSION} https://github.com/3DSGuy/Project_CTR /ctr     && cd /ctr/makerom && make deps -j"$(nproc)" && make -j"$(nproc)"     && cp bin/makerom /usr/local/bin/makerom

FROM devkitpro/devkitarm:latest AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential libffi-dev curl unzip ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# makerom (Project_CTR), pinned
ARG MAKEROM_VERSION=v0.19.0
RUN curl -fsSL -o /tmp/makerom.zip \
        "https://github.com/3DSGuy/Project_CTR/releases/download/makerom-${MAKEROM_VERSION}/makerom-${MAKEROM_VERSION}-ubuntu_x86_64.zip" \
    && unzip /tmp/makerom.zip -d /usr/local/bin && chmod +x /usr/local/bin/makerom && rm /tmp/makerom.zip

# A pinned Python and pinned packages, independent of the base image's
COPY --from=ghcr.io/astral-sh/uv:latest /uv /usr/local/bin/uv
COPY requirements.txt /tmp/requirements.txt
RUN uv venv --python 3.13 /opt/venv && uv pip install --python /opt/venv/bin/python -r /tmp/requirements.txt

ENV PATH="/opt/venv/bin:${PATH}" \
    DEVKITPRO=/opt/devkitpro \
    DEVKITARM=/opt/devkitpro/devkitARM \
    TEX3DS=/opt/devkitpro/tools/bin/tex3ds \
    MW3DS_MORROWIND_DIR=/morrowind \
    MW3DS_OUT=/app/out/data

# Your Morrowind install, copied from your machine
COPY --from=morrowind ["Data Files", "/morrowind/Data Files"]
COPY --from=morrowind ["Morrowind.ini", "/morrowind/Morrowind.ini"]

WORKDIR /app
COPY . /app

# 1. Convert the game data (the last "decoding" line names the sound file a crash stopped on)
RUN python tools/convert/level.py --world

# 2. The program
RUN make -j"$(nproc)"

COPY --from=makerom /usr/local/bin/makerom /usr/local/bin/makerom

# 3. The CIA (no loghost.txt: the game doesn't send its log over Wi-Fi, like make-cia.ps1 -NoNetLog)
RUN mkdir -p build/romfs && cp -a out/data build/romfs/data \
 && makerom -f cia -o build/mw3ds.cia -elf mw3ds.elf -rsf tools/build/mw3ds.rsf \
    -icon mw3ds.smdh -banner tools/build/banner.bnr -exefslogo -target t \
    -DAPP_TITLE=MW3DS -DAPP_PRODUCT_CODE=CTR-P-MW3D -DAPP_UNIQUE_ID=0xF3D50 \
    -DAPP_ROMFS=build/romfs -DAPP_CATEGORY=Application -DAPP_USE_ON_SD=true -DAPP_ENCRYPTED=false \
    -DAPP_MEMORY_TYPE=Application -DAPP_SYSTEM_MODE=64MB -DAPP_SYSTEM_MODE_EXT=124MB \
    -DAPP_CPU_SPEED=804MHz -DAPP_ENABLE_L2_CACHE=true -DAPP_VERSION_MAJOR=0

# Only the CIA leaves the build
FROM scratch AS cia
COPY --from=build /app/build/mw3ds.cia /mw3ds.cia
