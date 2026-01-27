FROM debian:12

WORKDIR /root

ARG DEBIAN_FRONTEND=noninteractive
ARG ESP_IDF_VERSION=v5.5.2
ARG ESP_QEMU_VERSION=esp-develop-9.2.2-20250817

# Update container
RUN apt-get update && apt-get upgrade -y

# Install dependencies
RUN apt-get install -y git wget flex bison gperf python3 python3-pip python3-setuptools python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 \
        libgcrypt20 libglib2.0-0 libpixman-1-0 libsdl2-2.0-0 libslirp0 \
        libglib2.0-dev libpixman-1-dev libgcrypt20-dev libslirp-dev libsdl2-dev

# Install ESP-IDF
ENV PATH="~/.local/bin:${PATH}"
RUN mkdir -p ~/esp && \
        cd ~/esp && git clone --recursive https://github.com/espressif/esp-idf.git -b ${ESP_IDF_VERSION} --depth 1 -j $(nproc) && \
        ln -s /usr/bin/python3 /usr/bin/python
RUN cd ~/esp/esp-idf && export IDF_GITHUB_ASSETS="dl.espressif.com/github_assets" && ./install.sh && /bin/bash -c "source ./export.sh"

# Install qemu
RUN cd ~/esp/esp-idf && python tools/idf_tools.py install qemu-riscv32 qemu-xtensa

# Or, build qemu from source
# RUN git clone https://github.com/espressif/qemu.git -b ${ESP_QEMU_VERSION} --depth 1 \
#         && cd qemu \
#         && mkdir -p build \
#         && cd build \
#         && ../configure --target-list=riscv32-softmmu \
#                 --enable-gcrypt \
#                 --enable-slirp \
#                 --enable-debug \
#                 --enable-sdl \
#                 --disable-strip --disable-user \
#                 --disable-capstone --disable-vnc \
#                 --disable-gtk \
#         && make -j $(nproc) vga=no \
#         && make install

RUN echo "alias idf='source /root/esp/esp-idf/export.sh'" >> .bashrc

# Start from a Bash prompt
CMD [ "/bin/bash" ]
