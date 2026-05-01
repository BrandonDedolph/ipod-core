# Convenience wrapper around Meson + Ninja.
#
# This is the human entry point. Real build logic lives in meson.build.
# The Go CLI (`core build hw` / `core build sim`) is a thin wrapper around
# these same targets — they should stay equivalent.

BUILD_HW  := build-hw
BUILD_SIM := build-sim

.PHONY: help hw sim clean realclean reconfigure-hw reconfigure-sim

help:
	@echo "Targets:"
	@echo "  make hw           Build the firmware image (ARM, iPod 5G/5.5G)"
	@echo "  make sim          Build the host simulator (SDL2)"
	@echo "  make clean        Remove build artifacts"
	@echo "  make realclean    Remove build directories entirely"
	@echo
	@echo "Build directories:"
	@echo "  $(BUILD_HW)/      ARM cross-build"
	@echo "  $(BUILD_SIM)/     host build"

hw: $(BUILD_HW)/build.ninja
	ninja -C $(BUILD_HW)

sim: $(BUILD_SIM)/build.ninja
	ninja -C $(BUILD_SIM)

$(BUILD_HW)/build.ninja:
	meson setup $(BUILD_HW) --cross-file cross/arm-none-eabi.txt -Dtarget=hw

$(BUILD_SIM)/build.ninja:
	meson setup $(BUILD_SIM) -Dtarget=sim

reconfigure-hw:
	meson setup --reconfigure $(BUILD_HW) --cross-file cross/arm-none-eabi.txt -Dtarget=hw

reconfigure-sim:
	meson setup --reconfigure $(BUILD_SIM) -Dtarget=sim

clean:
	@if [ -d $(BUILD_HW)  ]; then ninja -C $(BUILD_HW)  -t clean; fi
	@if [ -d $(BUILD_SIM) ]; then ninja -C $(BUILD_SIM) -t clean; fi

realclean:
	rm -rf $(BUILD_HW) $(BUILD_SIM)
