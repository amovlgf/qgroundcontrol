ARG BASE_IMAGE=ubuntu:22.04
FROM ${BASE_IMAGE}
ENV DEBIAN_FRONTEND=noninteractive
# Keep Ubuntu security updates; the delivery manifest records the built image ID.
# hadolint ignore=DL3008
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential g++-12 cmake ninja-build git python3 python3-pip python3-venv pkg-config patchelf ccache \
    libgl1-mesa-dev libegl1-mesa-dev libxkbcommon-dev libxkbcommon-x11-0 libxcb-cursor0 \
    libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 \
    libxcb-xinerama0 libxcb-xfixes0 libxcb-sync1 libxcb-xkb1 libxcb-util1 libdbus-1-dev \
    fontconfig fonts-noto-cjk libfontconfig1-dev libfreetype6-dev libpulse-dev libsdl2-dev libudev-dev libssl-dev libinput-dev \
    zlib1g-dev libzstd-dev libbz2-dev liblzma-dev libjpeg-dev libpng-dev libvulkan-dev \
    libx11-dev libx11-xcb-dev libxext-dev libxrender-dev xvfb xauth unzip curl ca-certificates file \
    libspeechd-dev libasound2-dev libnss3 libopengl0 mesa-utils \
    && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/qgc-python && /opt/qgc-python/bin/pip install --no-cache-dir \
    cmake==3.31.6 ninja==1.13.2 lxml==6.1.3 future==1.0.0 jinja2==3.1.6 packaging==26.3 \
    requests==2.34.2 setuptools==59.6.0 wheel==0.48.0 pymavlink==2.4.49 pyserial==3.5 \
    defusedxml==0.7.1 fastcrc==0.5.0 httpx==0.28.1 tomli==2.5.0
ENV PATH=/opt/qgc-python/bin:$PATH
ENV CC=gcc-12 CXX=g++-12
