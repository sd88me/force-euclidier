# armhf build env for Euclidier (Akai Force = ARM 32-bit hard-float), native under QEMU.
# Same approach/base as ~/force-acid/scripts/Dockerfile. See DESIGN.md.
FROM arm32v7/debian:stretch
RUN sed -i 's|deb.debian.org|archive.debian.org|g; /debian-security/d; /-updates/d' /etc/apt/sources.list \
    && apt-get -o Acquire::Check-Valid-Until=false update \
    && apt-get install -y --no-install-recommends --allow-unauthenticated g++ make libasound2-dev libncurses5-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
