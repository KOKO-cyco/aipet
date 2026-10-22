##
# @file Makefile
# @brief Host unit tests for the desktop-pet app.
#
# Runs the demo model on the development machine, where a wrong count or a setting
# that does not clamp is observable. On the device the same bugs are silent — a badge
# showing the wrong number, a slider that will not move past its end — so this is the
# only place they get caught.
#
#   make          build and run
#   make clean
#
# The model is deliberately free of LVGL and of the TuyaOpen runtime: pet_demo.c is
# the only source here, so the suite needs no display, no SDL and no platform layer.
# That is what makes it the fallback when the PC simulator cannot be run.
#
# The tests are built with ASan + UBSan and -Werror so that a buffer overrun or a
# signed overflow fails the run rather than corrupting memory quietly.
##

APP_DIR := ../..
ROOT    := $(APP_DIR)/../../..
PLATFORM := $(ROOT)/platform/LINUX/tuyaos_adapter
COMMON   := $(ROOT)/src/common

BUILD := .build

# The SDK's headers pull in two generated ones the host build has no way to produce:
# tuya_kconfig.h, which the simulator project keeps a hand-written stub of, and
# tuya_iot_config.h, which lives in src/common. Both include paths are needed, and the
# simulator's has to come first so its stub wins over any stale copy in a .build tree.
CFLAGS := -std=gnu99 -g -O1 -Wall -Wextra -Wno-unused-parameter -Werror \
          -fsanitize=address,undefined -fno-omit-frame-pointer \
          -DOPERATING_SYSTEM=100 -DLVGL_PC_SIMULATOR \
          -I $(APP_DIR)/include \
          -I $(ROOT)/examples/graphics/lvgl_simulator/include \
          -I $(PLATFORM)/include/utilities/include \
          -I $(COMMON)/include

SRCS := $(APP_DIR)/src/pet_demo.c

.PHONY: all clean

all: $(BUILD)/test_demo
	@echo "== pet_demo =="
	@$(BUILD)/test_demo

$(BUILD)/test_demo: $(APP_DIR)/tools/pet_test/test_demo.c $(SRCS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -rf $(BUILD)
