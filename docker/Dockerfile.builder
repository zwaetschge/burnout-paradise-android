# Clean-room builder image for the Burnout Paradise: The Ultimate Box ARM64 port.
# Contains ONLY open-source toolchains and fetched upstream sources:
# Android NDK r27c + SDK build-tools 36, Zig (ziglang), Python/Capstone/pefile,
# pcrecomp (pinned + patched), DXVK v3.1.1 (patched; Android libs + headless host
# d3d9 prebuilt here), Wine + lavapipe for the capture run. Zero game files.
FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive
ENV ANDROID_SDK_ROOT=/opt/android-sdk
ENV ANDROID_NDK_ROOT=/opt/android-ndk
ENV UB_VENV=/opt/ub-venv
ENV PCRECOMP_DIR=/opt/upstream/pcrecomp
ENV PATH="${UB_VENV}/bin:${ANDROID_SDK_ROOT}/cmdline-tools/latest/bin:${PATH}"

RUN dpkg --add-architecture i386 && apt-get update && apt-get install -y --no-install-recommends \
    bash build-essential ca-certificates cabextract curl git glslang-tools libvulkan1 \
    mesa-vulkan-drivers meson ninja-build openjdk-21-jdk-headless patch pkg-config \
    python3 python3-dev python3-venv unzip wine wine32:i386 wine64 xz-utils zip \
    && rm -rf /var/lib/apt/lists/*

# Python toolchain: lifter (pefile + capstone), Zig as host/Windows C compiler,
# optional image tools for the Remastered-vehicles converter.
RUN python3 -m venv "${UB_VENV}" \
    && pip install --no-cache-dir --upgrade pip \
    && pip install --no-cache-dir pefile==2024.8.26 capstone==5.0.9 unicorn==2.1.4 ziglang==0.17.0 \
       numpy pillow texture2ddecoder etcpak \
    && printf '#!/bin/sh\nexec %s/bin/python -m ziglang cc "$@"\n' "${UB_VENV}" > /usr/local/bin/zig-cc \
    && chmod +x /usr/local/bin/zig-cc

# Android SDK (platform 36 + build-tools 36.0.0) and NDK r27c
ARG CMDLINE_TOOLS_URL=https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
RUN mkdir -p "${ANDROID_SDK_ROOT}/cmdline-tools" \
    && curl -fsSL "${CMDLINE_TOOLS_URL}" -o /tmp/cmdline-tools.zip \
    && unzip -q /tmp/cmdline-tools.zip -d "${ANDROID_SDK_ROOT}/cmdline-tools" \
    && mv "${ANDROID_SDK_ROOT}/cmdline-tools/cmdline-tools" "${ANDROID_SDK_ROOT}/cmdline-tools/latest" \
    && rm -f /tmp/cmdline-tools.zip \
    && yes | sdkmanager --sdk_root="${ANDROID_SDK_ROOT}" --licenses >/dev/null \
    && sdkmanager --sdk_root="${ANDROID_SDK_ROOT}" "platforms;android-36" "build-tools;36.0.0"
ARG NDK_URL=https://dl.google.com/android/repository/android-ndk-r27c-linux.zip
RUN curl -fsSL "${NDK_URL}" -o /tmp/ndk.zip \
    && unzip -q /tmp/ndk.zip -d /opt && mv /opt/android-ndk-r27c "${ANDROID_NDK_ROOT}" && rm -f /tmp/ndk.zip

# DXVK v3.1.1 + Android/headless WSI patch: Android d3d9 libraries and a headless
# host d3d9 for the capture run
COPY patches/dxvk-android-wsi-and-build.patch /opt/patches/
COPY docker/dxvk/ /opt/dxvk-files/
RUN git clone --depth 1 --branch v3.1.1 --recurse-submodules --shallow-submodules \
       https://github.com/doitsujin/dxvk /opt/upstream/dxvk \
    && git -C /opt/upstream/dxvk apply /opt/patches/dxvk-android-wsi-and-build.patch \
    && sed "s#@NDK@#${ANDROID_NDK_ROOT}#g" /opt/dxvk-files/cross-android-arm64.txt.in > /opt/dxvk-files/cross-android-arm64.txt \
    && meson setup /opt/build/dxvk-android /opt/upstream/dxvk --cross-file /opt/dxvk-files/cross-android-arm64.txt \
       --prefix /opt/dxvk-android --libdir lib -Dbuildtype=release \
       -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false -Denable_dxgi=false \
    && ninja -C /opt/build/dxvk-android install \
    && meson setup /opt/build/dxvk-host /opt/upstream/dxvk --native-file /opt/dxvk-files/native-host-headless.txt \
       --prefix /opt/dxvk-host --libdir lib -Dbuildtype=release \
       -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false -Denable_dxgi=false \
    && ninja -C /opt/build/dxvk-host install \
    && rm -rf /opt/build

# Wine NLS tables for the code-page functions on the device (Wine data, LGPL)
RUN mkdir -p /opt/nls && for f in c_1252.nls sortdefault.nls; do \
      cp "$(find /usr/lib/wine /usr/lib/i386-linux-gnu/wine /usr/lib/x86_64-linux-gnu/wine /usr/share/wine -name "$f" 2>/dev/null | head -1)" /opt/nls/; \
    done && ls -la /opt/nls

# APK signing: APK Signature Scheme v2 in pure Python (tools/apk_sign_v2.py)
RUN pip install --no-cache-dir pycryptodome==3.23.0

# pcrecomp (static x86 lifter + recomp32 runtime headers), pinned and patched.
# Last on purpose: the port's lifter patches change more often than the toolchains above.
COPY patches/pcrecomp-*.patch /opt/patches/
RUN git clone https://github.com/sp00nznet/pcrecomp.git "${PCRECOMP_DIR}" \
    && git -C "${PCRECOMP_DIR}" checkout ef3f0da6e08a8eb7a3f869273f3df071ca8d7373 \
    && git -C "${PCRECOMP_DIR}" apply /opt/patches/pcrecomp-unsigned-dividend-shift.patch \
    && git -C "${PCRECOMP_DIR}" apply /opt/patches/pcrecomp-reentrant-startup-semantics.patch \
    && git -C "${PCRECOMP_DIR}" apply /opt/patches/pcrecomp-ub-extensions.patch

WORKDIR /repo
ENTRYPOINT ["/bin/bash"]
