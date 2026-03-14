FROM mcr.microsoft.com/vscode/devcontainers/cpp:debian

ARG ARDUINO_CLI_VERSION=1.4.1
ARG WOKWI_CLI_VERSION=0.26.1

# Install Arduino CLI
RUN mkdir -p /opt/arduino-cli \
    && cd /opt/arduino-cli \
    && wget https://github.com/arduino/arduino-cli/releases/download/v${ARDUINO_CLI_VERSION}/arduino-cli_${ARDUINO_CLI_VERSION}_Linux_64bit.tar.gz \
    && tar -xf arduino-cli_${ARDUINO_CLI_VERSION}_Linux_64bit.tar.gz \
    && rm -f arduino-cli_${ARDUINO_CLI_VERSION}_Linux_64bit.tar.gz \
    && ln -s /opt/arduino-cli/arduino-cli /usr/local/bin/arduino-cli \
    && arduino-cli version | grep -E "Version: ${ARDUINO_CLI_VERSION}"

# Install Wokwi CLI
RUN wget https://github.com/wokwi/wokwi-cli/releases/download/v${WOKWI_CLI_VERSION}/wokwi-cli-linuxstatic-x64 \
    && chmod +x wokwi-cli-linuxstatic-x64 \
    && mv wokwi-cli-linuxstatic-x64 /usr/local/bin/wokwi-cli \
    && wokwi-cli --version | grep -E "${WOKWI_CLI_VERSION}"

# Project specific section
# ========================
# Install extra board support for ATtiny85 globally
ENV ARDUINO_DIRECTORIES_DATA=/opt/arduino-cli 
RUN arduino-cli config init --additional-urls https://raw.githubusercontent.com/per1234/ReleaseScripts/refs/heads/provisional-package-index/package_drazzy.com_index.json \ 
    && arduino-cli core update-index \
    && arduino-cli core search attiny \
    && arduino-cli core install ATTinyCore:avr \
    && arduino-cli board listall ATtiny85 \ 
    && arduino-cli config dump --verbose

# Install any extern libraries here. Example:
# Install LiquidCrystal I2C library
# RUN arduino-cli lib install "LiquidCrystal I2C"
    