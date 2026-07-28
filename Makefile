# Path to the `modules` folder, which contains C modules and the python `scripts` folder.
USER_MODS_DIR = $(shell pwd)/modules

# Output folder for the compiled binary files
BUILD_DIR = $(shell pwd)/build_output

# Name of the docker image used for mpremote
MP_REMOTE = micropython-mpremote

# Volume used to store the mpy code
MPY_VOLUME = micropython_src_vol

# Version of microython to compile against (must be a valid github branch)
MPY_BRANCH = v1.28.0

# ESP-IDF Docker image and version
IDF_IMAGE = espressif/idf:v5.5.1

# Used for flashing and debugging, this is the device USB port config
PORT ?= /dev/ttyACM0
BAUD ?= 460800

# Defines which board and it's folder we're targetting
BOARD = LILYGO_T_DECK
BOARD_DIR = $(shell pwd)/boards

FILE ?= main.py

# Name of the docker image used for wikiconvert (has mwparserfromhell/requests
# preinstalled, so you don't need them on the host -- see utils/Dockerfile)
WIKICONVERT_IMAGE = vtos-wikiconvert

# Extra args passed through to wikiconvert.py, e.g:
#   make wikiconvert ARGS="--limit 500"
ARGS ?=

.PHONY: init build flash sync_files clean repl core_dump wikiconvert-image wikiconvert

init:
	docker build -t $(MP_REMOTE) .
	-docker volume rm -f $(MPY_VOLUME)
	docker volume create $(MPY_VOLUME)
	docker run --rm -v $(MPY_VOLUME):/opt/micropython alpine \
		sh -c "apk add --no-cache git && \
			cd /opt/micropython && \
			git clone --depth 1 --branch $(MPY_BRANCH) https://github.com/micropython/micropython.git . && \
			git submodule update --init --recursive || (rm -rf * && exit 1)"

build:
	@mkdir -p $(BUILD_DIR)
	rm -rf $(BUILD_DIR)/*
	docker run --rm \
		-v $(MPY_VOLUME):/opt/micropython \
		-v $(USER_MODS_DIR):/opt/all_modules \
		-v $(BUILD_DIR):/opt/external_build \
		-v $(BOARD_DIR):/opt/boards \
		$(IDF_IMAGE) \
		/bin/bash -c "cp -r /opt/boards/* /opt/micropython/ports/esp32/boards/ && \
			cp /opt/all_modules/idf_component.yml /opt/micropython/ports/esp32/main/idf_component.yml && \
			mkdir -p /opt/esp/idf/components/esp_hw_support/mspi_timing_tuning/port/esp32s3/include && \
			source /opt/esp/idf/export.sh && \
			make -C /opt/micropython/mpy-cross && \
			make -C /opt/micropython/ports/esp32 \
				BOARD=$(BOARD) \
				USER_C_MODULES=/opt/all_modules \
				FROZEN_MANIFEST=/opt/all_modules/manifest.py && \
			cp /opt/micropython/ports/esp32/build-$(BOARD)/firmware.bin /opt/external_build/ && \
			cp /opt/micropython/ports/esp32/build-$(BOARD)/micropython.bin /opt/external_build/ && \
			chown -R $(shell id -u):$(shell id -g) /opt/external_build/."

flash:
	docker run --rm --privileged \
		--device=$(PORT):$(PORT) \
		-v $(BUILD_DIR):/flash_dir \
		$(IDF_IMAGE) \
		/bin/bash -c "esptool.py -p $(PORT) -b $(BAUD) --chip esp32s3 erase_flash && \
			esptool.py -p $(PORT) -b $(BAUD) --chip esp32s3 write_flash 0x0 /flash_dir/firmware.bin"

sync_files:
	docker run --rm -it \
		--privileged \
		-v /dev/bus/usb:/dev/bus/usb \
		-v $(USER_MODS_DIR):/opt/all_modules \
		--device=$(PORT):$(PORT) \
		$(MP_REMOTE) \
		mpremote connect $(PORT) cp -r /opt/all_modules/scripts/ :

sync_file:
	docker run --rm -it \
		--privileged \
		-v /dev/bus/usb:/dev/bus/usb \
		-v $(USER_MODS_DIR):/opt/all_modules \
		--device=$(PORT):$(PORT) \
		$(MP_REMOTE) \
		mpremote connect $(PORT) cp /opt/all_modules/scripts/$(FILE) :$(FILE)

clean:
	@echo "Cleaning mpy-cross, ESP32 build cache, and local output..."
	docker run --rm -v $(MPY_VOLUME):/opt/micropython $(IDF_IMAGE) \
		/bin/bash -c "make -C /opt/micropython/mpy-cross clean && \
			rm -rf /opt/micropython/ports/esp32/build-* && \
			rm -rf /opt/micropython/ports/esp32/boards/$(BOARD)"
	rm -rf $(BUILD_DIR)/*

repl:
	docker run --rm -it \
		--privileged \
		-v /dev/bus/usb:/dev/bus/usb \
		--device=$(PORT):$(PORT) \
		$(MP_REMOTE) \
		mpremote connect $(PORT) repl

core_dump:
	docker run --rm -it --privileged \
		-v /dev/bus/usb:/dev/bus/usb \
		--device=$(PORT):$(PORT) \
		-v $(MPY_VOLUME):/opt/micropython \
		$(IDF_IMAGE) \
		/bin/bash -c "source /opt/esp/idf/export.sh && \
			espcoredump.py --chip esp32s3 --port $(PORT) \
			info_corefile --core-format elf \
			/opt/micropython/ports/esp32/build-$(BOARD)/micropython.elf"

wikiconvert-image:
	docker build -t $(WIKICONVERT_IMAGE) utils/

wikiconvert: wikiconvert-image
	docker run --rm \
		-v $(shell pwd)/utils:/work \
		$(WIKICONVERT_IMAGE) \
		/bin/bash -c "python3 wikiconvert.py $(ARGS) && \
			chown -R $(shell id -u):$(shell id -g) /work"

debug:
	docker run --rm -it --privileged \
		-v /dev/bus/usb:/dev/bus/usb \
		--device=$(PORT):$(PORT) \
		-v $(MPY_VOLUME):/opt/micropython \
		$(IDF_IMAGE) \
		/bin/bash -c "source /opt/esp/idf/export.sh && \
			openocd \
				-c 'source [find interface/esp_usb_jtag.cfg]' \
				-c 'espusbjtag vid_pid 0x303a 0x4001' \
				-c 'source [find target/esp32s3.cfg]' \
				>/tmp/openocd.log 2>&1 & \
			sleep 8 && \
			echo '--- Attaching now, board should still be healthy. Trigger the crash from the device keyboard, THEN Ctrl-C here and run: thread apply all bt ---' && \
			xtensa-esp32s3-elf-gdb /opt/micropython/ports/esp32/build-$(BOARD)/micropython.elf \
				-ex 'target remote :3333' \
				-ex 'continue'; \
			cat /tmp/openocd.log"
