# Build environment of the Switch port (tools/switch/build.sh): devkitA64, libnx and the portlibs the
# runtime links, plus the Vulkan headers matching the NVK driver object (docs/switch.md).
FROM devkitpro/devkita64:latest

RUN apt-get update && \
    apt-get install -y --no-install-recommends cmake ninja-build python3 git ca-certificates && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

RUN dkp-pacman -Syu --noconfirm && \
    dkp-pacman -S --needed --noconfirm libnx switch-tools switch-cmake switch-pkg-config \
        switch-zlib switch-libzstd switch-lz4 switch-libdrm_nouveau switch-libexpat && \
    dkp-pacman -Scc --noconfirm

# devkitPro ships no libelf. NVK's only libelf user (VK_NVX_binary_import) is compiled out on the
# Switch, but the link line names libelf.a: provide an empty archive.
RUN cd /tmp && echo 'void __wwhd_libelf_stub(void) {}' > elfstub.c && \
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc -c elfstub.c -o elfstub.o && \
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar rcs /opt/devkitpro/portlibs/switch/lib/libelf.a elfstub.o && \
    rm elfstub.c elfstub.o

# Vulkan-Headers of the NVK build (VK_HEADER_VERSION 354)
RUN git clone --depth 1 --branch v1.4.354 https://github.com/KhronosGroup/Vulkan-Headers.git /opt/vulkan-headers

ENV DEVKITPRO=/opt/devkitpro
ENV PATH="/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:${PATH}"
WORKDIR /src
