# The converter image: turns a player's own Morrowind install into a CIA on Linux, the Steam Deck (podman) or anywhere
# else with podman or Docker. tools/release/build_image.py stages its build context (app/, licenses/, this file); the
# release workflow builds it with the engine CI built and pushes it to ghcr.io/3ds-conspiracy/mw3ds-converter.
# Nothing from Morrowind is in it: the player mounts their install at /morrowind. See tools/release/container.py.

# tex3ds, as devkitPro's image (the one CI builds the engine in) carries it
FROM devkitpro/devkitarm AS devkitpro

# makerom, Project_CTR's Linux build
FROM python:3.13-slim-trixie AS makerom
ARG MAKEROM_VERSION=v0.19.0
ADD https://github.com/3DSGuy/Project_CTR/releases/download/makerom-${MAKEROM_VERSION}/makerom-${MAKEROM_VERSION}-ubuntu_x86_64.zip /tmp/makerom.zip
RUN python -c "import zipfile; zipfile.ZipFile('/tmp/makerom.zip').extractall('/tools')" && chmod +x /tools/makerom

# (Debian 13: makerom's Linux build needs glibc 2.38; devkitPro's tex3ds, built on Debian 12, needs only libc and
# libstdc++ and runs on it)
FROM python:3.13-slim-trixie
LABEL org.opencontainers.image.title="MW3DS Converter" \
      org.opencontainers.image.description="Turns your own Morrowind install into an MW3DS CIA for a New 3DS or Old 3DS" \
      org.opencontainers.image.source="https://github.com/3ds-conspiracy/mw3ds" \
      org.opencontainers.image.licenses="GPL-3.0-or-later"
COPY --from=devkitpro /opt/devkitpro/tools/bin/tex3ds /opt/mw3ds/bin/tex3ds
COPY --from=makerom /tools/makerom /opt/mw3ds/bin/makerom
COPY app/requirements.txt /opt/mw3ds/requirements.txt
# (imageio-ffmpeg's own ffmpeg is taken out, as in the Windows converter: container.py downloads it on first run)
RUN pip install --no-cache-dir -r /opt/mw3ds/requirements.txt \
 && rm -f /usr/local/lib/python3.13/site-packages/imageio_ffmpeg/binaries/ffmpeg-* \
 && /opt/mw3ds/bin/tex3ds --version \
 && (/opt/mw3ds/bin/makerom 2>&1 || true) | grep -i makerom
COPY licenses /opt/mw3ds/licenses
COPY app /opt/mw3ds/app
ENV TEX3DS=/opt/mw3ds/bin/tex3ds \
    MW3DS_CTR_TOOLS_DIR=/opt/mw3ds/bin \
    MW3DS_APP=/opt/mw3ds/app \
    MW3DS_MORROWIND_DIR=/morrowind \
    MW3DS_OUT_DIR=/out \
    PYTHONUNBUFFERED=1
WORKDIR /out
ENTRYPOINT ["python", "/opt/mw3ds/app/tools/release/container.py"]
