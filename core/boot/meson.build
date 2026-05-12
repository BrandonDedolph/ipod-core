# Bootloader entry point.
#
# crt0.S sets up the stack, zeros .bss, and branches to kernel_main. The
# linker script (linker.ld) lays out memory; the top-level meson.build
# passes it to the executable target's link_args.

boot_sources = files('crt0.S')
boot_linker_script = files('linker.ld')
