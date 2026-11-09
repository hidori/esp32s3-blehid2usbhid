# arduino-cli with the project's configuration file
ARDUINO_CLI = arduino-cli --config-file arduino-cli.yaml

# Core platforms used by the project
CORES ?= \
	m5stack:esp32 \
	esp32:esp32

# Libraries used by the project (e.g. lvgl@8.3.11)
LIBS ?=

# Target boards
BOARDS = m5stack-atoms3u m5stack-atoms3 m5stack-atoms3r xiao-esp32s3

# FQBN for each target board (USB-OTG mode is required for USB HID)
FQBN_m5stack-atoms3u = m5stack:esp32:m5stack_atoms3:USBMode=default
FQBN_m5stack-atoms3  = m5stack:esp32:m5stack_atoms3:USBMode=default
FQBN_m5stack-atoms3r = m5stack:esp32:m5stack_atoms3r:USBMode=default
FQBN_xiao-esp32s3    = esp32:esp32:XIAO_ESP32S3:USBMode=default

# Serial port for upload / monitor (e.g. make upload/m5stack-atoms3 PORT=/dev/ttyACM0)
PORT ?=

.PHONY: all install tools/install core/install lib/install build $(addprefix build/,$(BOARDS)) $(addprefix upload/,$(BOARDS)) monitor clean

# Default target
all: $(addprefix build/,$(BOARDS))

# Install the core platforms and the libraries
install: core/install lib/install

# Install required tools
tools/install:
	@echo "Installing required tools..."
	mise install || { echo "Failed to install required tools"; exit 1; }

# Install the core platforms
core/install:
ifeq ($(strip $(CORES)),)
	@echo "No cores defined. Skipping core/install."
else
	$(ARDUINO_CLI) core update-index
	$(ARDUINO_CLI) core install $(CORES)
endif

# Install the libraries
lib/install:
ifeq ($(strip $(LIBS)),)
	@echo "No libraries defined. Skipping lib/install."
else
	$(ARDUINO_CLI) lib update-index
	$(ARDUINO_CLI) lib install $(LIBS)
endif

# Build the sketch into build/<board>/
$(addprefix build/,$(BOARDS)): build/%:
	$(ARDUINO_CLI) compile -b $(FQBN_$*) --output-dir $@ .

# Upload build/<board>/ to the board
$(addprefix upload/,$(BOARDS)): upload/%: build/%
	$(ARDUINO_CLI) upload -b $(FQBN_$*) -p $(PORT) --input-dir build/$* .

# Open the serial monitor
monitor:
	$(ARDUINO_CLI) monitor -p $(PORT)

# Remove build outputs
clean:
	rm -rf build
