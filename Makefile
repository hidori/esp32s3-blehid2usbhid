# Sketch name (same as the sketch folder name)
SKETCH = $(notdir $(CURDIR))

# Target boards
BOARDS = m5stack-atoms3

# FQBN for each target board
FQBN_m5stack-atoms3 = m5stack:esp32:m5stack_atoms3

# Board options for each target board (USB-OTG mode is required for USB HID)
BOARD_OPTIONS_m5stack-atoms3 = USBMode=default

# Core platforms used by the project
CORES ?= \
	m5stack:esp32

# Libraries used by the project (e.g. lvgl@8.3.11)
LIBS ?=

# arduino-cli with the project's configuration file
ARDUINO_CLI = arduino-cli --config-file arduino-cli.yaml

# Export directory of arduino-cli for the board (build/<FQBN with ':' replaced by '.'>/)
BUILD_DIR = build/$(subst :,.,$(FQBN_$*))

# Serial port for upload (default: /dev/ttyACM0)
UPLOAD_PORT ?= /dev/ttyACM0

# Upload <directory> to the board: $(call upload_arduino,<fqbn>,<board options>,<directory>)
define upload_arduino
$(ARDUINO_CLI) upload --fqbn $(1) --board-options $(2) --port $(UPLOAD_PORT) --input-dir $(3) .
endef

.PHONY: all install tools/install core/install lib/install $(addprefix build/,$(BOARDS)) $(addprefix upload/,$(BOARDS)) clean

# Default target
all: $(addprefix build/,$(BOARDS))

# Install the required tools, the core platforms and the libraries
install: tools/install core/install lib/install

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

# Build the sketch and export the binaries into $(BUILD_DIR)
$(addprefix build/,$(BOARDS)): build/%:
	$(ARDUINO_CLI) compile --fqbn $(FQBN_$*) --board-options $(BOARD_OPTIONS_$*) --export-binaries .

# Upload $(BUILD_DIR) to the board
$(addprefix upload/,$(BOARDS)): upload/%:
	$(call upload_arduino,$(FQBN_$*),$(BOARD_OPTIONS_$*),$(BUILD_DIR))

# Remove build outputs and installed libraries
clean:
	rm -rf build .arduino
