# Top-level Meson build for the core firmware.
#
# Two outputs from one source tree:
#   - core.ipod  : ARM cross-compile (the firmware that ships on device)
#   - core-sim   : host build, SDL-backed (for development on Linux/macOS)
#
# Pick the target with: meson setup build-hw  --cross-file cross/arm-none-eabi.txt -Dtarget=hw
#                       meson setup build-sim                                       -Dtarget=sim
#
# This file is intentionally minimal at the moment — subsystems land in
# subsequent PRs and will register themselves via subdir() calls below.

project(
  'core',
  'c',
  version: '0.0.1',
  license: 'Apache-2.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=3',
    'werror=true',
    'b_lto=true',
    'optimization=s',          # -Os; size-optimized
  ],
)

target = get_option('target')
cc = meson.get_compiler('c')

# Build stamp, screen-only: the boot screen prints it bottom-right in the
# border colour, and Boot Details prints it in the header. `git describe
# --tags --always --dirty --abbrev=7` — the --dirty is the point, since device
# images are routinely built from an uncommitted tree and the screen should say
# so, and --tags makes LIGHTWEIGHT tags count too (plain `git describe` only
# sees annotated ones, so a `git tag v0.1.0` would have been invisible here).
# Never printed on the UART (the clicky boot golden matches those lines
# exactly), so this cannot move a test. Without git (or outside a checkout)
# meson substitutes the fallback rather than failing.
core_version_h = vcs_tag(
  input: 'kernel/core_version.h.in',
  output: 'core_version.h',
  command: ['git', 'describe', '--tags', '--always', '--dirty', '--abbrev=7'],
  fallback: 'unknown',
  replace_string: '@VCS_TAG@',
)

# The release version — the NEAREST tag alone, no commit count and no hash, so
# Settings > About can print a chip that reads "Core v0.1.0". Separate from the
# build id above because vcs_tag does exactly one substitution per call, and
# separate from a run_command() because run_command only runs at CONFIGURE
# time: vcs_tag re-runs on every `ninja`, so a freshly created tag shows up in
# the next build without a reconfigure.
#
# `git describe --tags --abbrev=0` FAILS in a repository with no tags at all
# (it exits non-zero and prints nothing). That is the state of this tree until
# the first tag is cut, and it must not break the build: meson's vcs_tag
# catches the failure and substitutes the fallback, so an untagged tree builds
# and reports v0.0.0.
core_version_tag_h = vcs_tag(
  input: 'kernel/core_version_tag.h.in',
  output: 'core_version_tag.h',
  command: ['git', 'describe', '--tags', '--abbrev=0'],
  fallback: 'v0.0.0',
  replace_string: '@VCS_TAG@',
)

# Warning flags that only exist on newer compilers go through
# get_supported_arguments(), which probes the ACTUAL compiler in use. This
# matters for -Wno-error= in particular: passing -Wno-error=<name-the-compiler-
# does-not-know> makes gcc emit "'-Werror=' argument ... is not valid", and
# under -Werror that is itself a hard error — so an unguarded flag added for
# gcc 16 would break the build on gcc 14. The toolchain floor lives in
# .tool-versions; this keeps older-but-supported toolchains working anyway.
# Currently empty: the gcc-16 -Wunterminated-string-initialization demotion
# that lived here is gone. It was added during the 14->16 upgrade for the
# exactly-sized `char hex[16] = "0123456789ABCDEF"` lookup tables; uart.c now
# declares that table unsized (17 B incl. NUL, hal/hw/uart.c) and lcd.c never
# had one, so no first-party file trips the diagnostic any more. Leaving the
# demotion in place would have let a genuinely truncated initializer — a char
# array with no room for its NUL, handed to something that expects a C string
# — through -Werror unnoticed.
#
# Keep the probing mechanism itself: passing an unguarded -Wno-error=<flag the
# compiler does not know> is itself an error under -Werror, so any future
# version-dependent suppression belongs in this list rather than inline.
optional_c_args = cc.get_supported_arguments([])

# Common warning/safety flags
# (warning_level=3 already implies -Wall -Wextra -Wpedantic; we add specifics.)
common_c_args = optional_c_args + [
  '-ffunction-sections',
  '-fdata-sections',
  '-Wundef',
  '-Wshadow',
  # NOTE: -Wno-unused-parameter used to live here ("too noisy in driver stubs
  # during scaffold"). The scaffold is gone and the suppression was hiding
  # genuine forgot-to-use-the-argument bugs across all first-party code, so it
  # is no longer global. The vendored codec blocks below still list it in their
  # own suppression sets, which is where it belongs.
  #
  # Version-dependent suppressions live in optional_c_args above, which probes
  # the compiler before adding them.
]

common_link_args = [
  '-Wl,--gc-sections',
]

if target == 'hw'
  # ARM cross-build. Toolchain + ARM-specific flags live in
  # cross/arm-none-eabi.txt under [built-in options]; we add only the
  # freestanding-correctness flags that don't belong in the cross file.
  common_c_args += [
    '-fno-builtin',
    '-fno-common',
    '-fno-stack-protector',
    # Stack-depth guard. There is no stack probe and no guard page on this
    # target: the supervisor stack is 0x40017C00 growing down with only the
    # 1 KB banked IRQ stack above it, so one oversized frame silently corrupts
    # memory instead of faulting. 1024 bytes/frame is well above anything
    # legitimate here (the deepest real frames are a few hundred bytes of
    # dirent/sector scratch) and well below the available stack, so it catches
    # a new multi-KB local array at compile time. Raise it only with a note
    # about where the space came from.
  ] + cc.get_supported_arguments(['-Wframe-larger-than=1024'])
  add_project_arguments(common_c_args, language: 'c')
  add_project_link_arguments(common_link_args, language: 'c')
  message('Configured for hardware target (ARM, iPod 5G/5.5G)')

elif target == 'sim'
  # Host build. SDL2 provides LCD window + audio out + input.
  sdl2_dep = dependency('sdl2', required: true)
  add_project_arguments(common_c_args, language: 'c')
  add_project_link_arguments(common_link_args, language: 'c')
  message('Configured for sim target (host, SDL2)')

else
  error('Unknown target "@0@" (use -Dtarget=hw or -Dtarget=sim)'.format(target))
endif

# ---- pvmp3 source manifest -------------------------------------------------
#
# The vendored MP3 decoder (codecs/pvmp3/upstream/, AOSP PacketVideo,
# Apache-2.0 — see codecs/pvmp3/README.md). Listed file by file rather than
# globbed so that a re-vendor which adds or drops one is a visible diff, and
# defined HERE rather than in codecs/meson.build because both targets need it:
# the hw branch below never enters that subdir.
#
# Four translation units are special. Under -DPV_ARM_GCC_V4 their bodies are
# #if'd out in favour of the hand-written ARMv4 assembly in upstream/asm/, so
# on hw they would be EMPTY translation units (which -Wpedantic rejects) and
# the .s files are listed instead. The host build has no assembly and compiles
# the C.
pvmp3_asm_replaced_c = files(
  'codecs/pvmp3/upstream/pvmp3_dct_16.c',
  'codecs/pvmp3/upstream/pvmp3_dct_9.c',
  'codecs/pvmp3/upstream/pvmp3_mdct_18.c',
  'codecs/pvmp3/upstream/pvmp3_polyphase_filter_window.c',
)
pvmp3_arm_asm = files(
  'codecs/pvmp3/upstream/asm/pvmp3_dct_16_gcc.s',
  'codecs/pvmp3/upstream/asm/pvmp3_dct_9_gcc.s',
  'codecs/pvmp3/upstream/asm/pvmp3_mdct_18_gcc.s',
  'codecs/pvmp3/upstream/asm/pvmp3_polyphase_filter_window_gcc.s',
)
pvmp3_common_c = files(
  'codecs/pvmp3/upstream/pvmp3_alias_reduction.c',
  'codecs/pvmp3/upstream/pvmp3_crc.c',
  'codecs/pvmp3/upstream/pvmp3_dct_6.c',
  'codecs/pvmp3/upstream/pvmp3_decode_header.c',
  'codecs/pvmp3/upstream/pvmp3_decode_huff_cw.c',
  'codecs/pvmp3/upstream/pvmp3_dequantize_sample.c',
  'codecs/pvmp3/upstream/pvmp3_equalizer.c',
  'codecs/pvmp3/upstream/pvmp3_framedecoder.c',
  'codecs/pvmp3/upstream/pvmp3_get_main_data_size.c',
  'codecs/pvmp3/upstream/pvmp3_get_scale_factors.c',
  'codecs/pvmp3/upstream/pvmp3_get_side_info.c',
  'codecs/pvmp3/upstream/pvmp3_getbits.c',
  'codecs/pvmp3/upstream/pvmp3_huffman_decoding.c',
  'codecs/pvmp3/upstream/pvmp3_huffman_parsing.c',
  'codecs/pvmp3/upstream/pvmp3_imdct_synth.c',
  'codecs/pvmp3/upstream/pvmp3_mdct_6.c',
  'codecs/pvmp3/upstream/pvmp3_mpeg2_get_scale_data.c',
  'codecs/pvmp3/upstream/pvmp3_mpeg2_get_scale_factors.c',
  'codecs/pvmp3/upstream/pvmp3_mpeg2_stereo_proc.c',
  'codecs/pvmp3/upstream/pvmp3_normalize.c',
  'codecs/pvmp3/upstream/pvmp3_poly_phase_synthesis.c',
  'codecs/pvmp3/upstream/pvmp3_reorder.c',
  'codecs/pvmp3/upstream/pvmp3_seek_synch.c',
  'codecs/pvmp3/upstream/pvmp3_stereo_proc.c',
  'codecs/pvmp3/upstream/pvmp3_tables.c',
)

# Our own MP3 layer on top of it: the decoder_ops wrapper, the framing/Xing/
# seek maths and the ID3 reader. First-party, so it stays under -Werror in a
# static_library of its own while the vendored objects do not.
pvmp3_wrapper_c = files(
  'codecs/pvmp3/mp3.c',
  'codecs/pvmp3/mp3_frame.c',
  'codecs/pvmp3/id3_meta.c',
)

subdir('hal')         # hardware abstraction layer (sim or hw backend)

if target == 'hw'
  # Bare-metal ARM build. boot/ + kernel/ are all that's freestanding-
  # clean today; codecs, audio engine, tagcache, and UI still pull in
  # host libc and land in subsequent PRs as they're brought up.
  subdir('boot')
  subdir('kernel')
  subdir('fs')
  subdir('lib')

  # FLAC decoder (dr_flac) built freestanding for the target: no libc — a
  # static arena backs allocation and our lib/mem.c provides memcpy/memset
  # (see codecs/dr_flac/flac.c's CORE_FREESTANDING block). Warnings are
  # suppressed and werror disabled for the vendored single-header source,
  # which we don't own; arena.c + mem.c are ours and stay under -Werror.
  codec_warn_suppress = [
    '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
    '-Wno-sign-compare', '-Wno-shadow', '-Wno-undef', '-Wno-cast-align',
    '-Wno-implicit-fallthrough', '-Wno-misleading-indentation',
  ]
  codec_inc = include_directories('codecs', 'codecs/dr_flac', 'codecs/pvmp3', 'lib')
  flac_hw_lib = static_library(
    'core_codec_flac_hw',
    sources: ['codecs/dr_flac/flac.c'],
    c_args: ['-DCORE_FREESTANDING'] + codec_warn_suppress,
    include_directories: codec_inc,
    override_options: ['werror=false'],
  )

  # MP3 decoder: AOSP's pvmp3, built freestanding for the target. Unlike
  # dr_mp3 before it, this one is PURE INTEGER — its Q-format constants fold at
  # compile time, so it needs no __aeabi_f* soft-float helper at all. (Compile
  # any of these translation units on its own and `nm -u` lists none; the float
  # symbols still in core.map are dr_flac's. LTO hides that from a check on the
  # linked image, so it is documented in codecs/README.md rather than gated.)
  # -DPV_ARM_GCC_V4 selects the smull/smlal inline ops and the four
  # hand-written ARMv4 routines in upstream/asm/, whose C counterparts are
  # therefore left out of the source list. Vendored: warnings suppressed,
  # werror off.
  pvmp3_hw_lib = static_library(
    'core_codec_pvmp3_hw',
    sources: pvmp3_common_c + pvmp3_arm_asm,
    c_args: ['-DCORE_FREESTANDING', '-DPV_ARM_GCC_V4'] + codec_warn_suppress + [
      '-Wno-type-limits',
    ],
    include_directories: codec_inc,
    override_options: ['werror=false'],
  )

  # Our MP3 layer over it (wrapper + framing/Xing/seek + ID3 reader). OURS, so
  # unlike the block above it stays under -Werror.
  mp3_hw_lib = static_library(
    'core_codec_mp3_hw',
    sources: pvmp3_wrapper_c,
    c_args: ['-DCORE_FREESTANDING', '-DPV_ARM_GCC_V4'],
    include_directories: codec_inc,
  )

  # Read-ahead byte-source shim (ours, not vendored): collapses a decoder's
  # many tiny reads into a few large disk reads. Built freestanding so it pulls
  # lib/mem.h's memcpy (via CORE_FREESTANDING); stays under -Werror since we own
  # it and it's warning-clean.
  readahead_hw_lib = static_library(
    'core_readahead_hw',
    sources: ['codecs/readahead.c'],
    c_args: ['-DCORE_FREESTANDING'],
    include_directories: codec_inc,
  )

  # FLAC metadata reader (ours, not vendored): parses a FLAC's STREAMINFO +
  # VORBIS_COMMENT blocks up front so the UI can show real title/artist/album/
  # genre/track#/year + duration without decoding audio. Pulls bytes through a
  # decoder_source_t (same fat_src + read-ahead path as the streaming decoder).
  # Freestanding (no libc/libm/malloc — all buffers are stack/struct); reaches
  # decoder.h via a relative include (no extra dirs). Same flac_meta.c the host
  # flac_meta_test compiles; stays under -Werror since we own it.
  flac_meta_hw_lib = static_library(
    'core_flac_meta_hw',
    sources: ['codecs/flac_meta.c'],
    c_args: ['-DCORE_FREESTANDING'],
    include_directories: codec_inc,
  )

  # The tag-text policy the FLAC and ID3 readers share (ours): what bytes are
  # allowed into a display field and how a field truncates. One library rather
  # than a copy in each reader, and a library of its own rather than a member
  # of one of them, so neither archive can shadow the other's copy at link.
  meta_text_hw_lib = static_library(
    'core_meta_text_hw',
    sources: ['codecs/meta_text.c'],
    c_args: ['-DCORE_FREESTANDING'],
    include_directories: codec_inc,
  )

  # Anti-skip disk buffer (ours, not vendored): a MB-scale watermarked buffer
  # that reads ahead in bursts so the drive head parks between refills instead of
  # streaming continuously (the "constant arm movement" fix). Sits under the
  # read-ahead shim. Freestanding (CORE_FREESTANDING → lib/mem.h memcpy);
  # warning-clean under -Werror. Same diskbuf.c the host diskbuf_test compiles.
  diskbuf_hw_lib = static_library(
    'core_diskbuf_hw',
    sources: ['codecs/diskbuf.c'],
    c_args: ['-DCORE_FREESTANDING'],
    include_directories: codec_inc,
  )

  # Freestanding antialiased text renderer (ours): draws Nunito type from the
  # pre-rasterized atlas/*.h data into an RGB565 framebuffer with a gamma-correct
  # integer blend — no libc/libm/malloc. Same text.c the host text_test compiles;
  # links into core.elf so the on-device UI can draw real type. Uses only relative
  # includes (ui/atlas.h -> ../hal/hal.h), so no extra include dirs; stays
  # under -Werror since we own it and it's warning-clean.
  text_hw_lib = static_library(
    'core_text_hw',
    sources: ['ui/text.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Freestanding nearest-neighbor RGB565 downscaler (ours): shrinks a loaded
  # folder.art to a list chip / album-detail cover when no dedicated folder.thm
  # is on disk. Pure integer math — no libc/libm/malloc, no allocation. Same
  # thumb.c the host thumb_test compiles; links into core.elf. Only relative
  # includes (thumb.h), so no extra include dirs; stays under -Werror.
  thumb_hw_lib = static_library(
    'core_thumb_hw',
    sources: ['ui/thumb.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Shared list/panel chrome (ours): the header, list rows, selection bar,
  # scrollbar, scroll window, rounded rect and text wrappers that every screen
  # is built from. EXTRACTED OUT OF kernel/main.c, where it was file-local
  # static and therefore uncompilable on the host (main.c is hw-only), and
  # where ui/screen_settings.c kept a hand-maintained second copy that had
  # already drifted. Pure: console.h + text.h + palette.h, no hardware — the
  # marquee's time source is injected via ui_set_scroll_text(). Same chrome.c
  # the host chrome_test compiles.
  chrome_hw_lib = static_library(
    'core_chrome_hw',
    sources: ['ui/chrome.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Wheel acceleration + the A-Z locator (ours): the velocity curve, the
  # idle reset, the letter-mode latch and hold window, the tick accumulator
  # and the letter stepping every list is navigated with. EXTRACTED OUT OF
  # kernel/main.c, where it was file-local static and could only be checked by
  # spinning the wheel. Pure integer logic; its clock, its row-initial source
  # and its click are injected from kernel_main (wheel_set_*), the seam
  # chrome.c uses for the marquee. pp5022.h for CW_WHEEL_SENSITIVITY only.
  # Same wheel.c the host wheel_test compiles.
  wheel_hw_lib = static_library(
    'core_wheel_hw',
    sources: ['ui/wheel.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Where each letter starts on an alphabetised list (ours): the run index
  # every long list's A-Z plate and letter stepping is answered out of. It is
  # what lets the locator work on Artists / Albums / Playlists / Genres and
  # not just Songs — the per-row initial is a different KEY on each of them,
  # so it is injected and the index is built once per list instead of walked
  # per detent. Pure integer logic over an injected callback, no globals.
  # Same letterindex.c the host letterindex_test compiles.
  letterindex_hw_lib = static_library(
    'core_letterindex_hw',
    sources: ['ui/letterindex.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Tap-vs-hold press-length arbitration (ours): the machine that decides
  # whether PLAY was tapped (pause) or held (sleep), fed live button state
  # once per main-loop pass. Extracted from kernel/main.c so the host can
  # test it; same keyhold.c the host keyhold_test compiles.
  keyhold_hw_lib = static_library(
    'core_keyhold_hw',
    sources: ['ui/keyhold.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # The three press-length GESTURES built on keyhold (ours): PLAY on a list
  # title (which screens start a queue and which pause), RIGHT/LEFT held (the
  # skip-vs-seek arbiter, its aim and its ramp table), and MENU's jump-home
  # threshold. Kept out of kernel/main.c so the policy and the timing are
  # testable off-device; same gesture.c the host gesture_test compiles.
  gesture_hw_lib = static_library(
    'core_gesture_hw',
    sources: ['ui/gesture.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # The sleep timer's countdown (ours): a wrap-safe minute accumulator over
  # the 1 MHz USEC_TIMER, fed once per main-loop pass. Out of kernel/main.c so
  # the host can run a whole 120-minute countdown across both 32-bit clock
  # wraps; same sleeptimer.c the host sleeptimer_test compiles.
  sleeptimer_hw_lib = static_library(
    'core_sleeptimer_hw',
    sources: ['ui/sleeptimer.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Pause-on-unplug policy (ours): the edge detector over the debounced
  # headphone-jack level that decides whether a pull-out pauses. Extracted
  # from kernel/main.c for the same reason keyhold.c was — it could not be
  # tested where it lived; same jackwatch.c the host jackwatch_test compiles.
  jackwatch_hw_lib = static_library(
    'core_jackwatch_hw',
    sources: ['ui/jackwatch.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Music-library model (ours; library/, not to be confused with lib/, the
  # freestanding libc). names.c is the name handling the library is built on:
  # the folded locator hash that binds a CORELIB.IDX record to its file, the
  # display-name copy every row and queue entry is made by, the folder-name
  # split, extension classification, and the decimal/time formatters. EXTRACTED
  # OUT OF kernel/main.c, where it was file-local static and therefore
  # uncompilable on the host — which is why tests/kernel kept a verbatim copy
  # of name_hash() with a Python script diffing it against main.c. Pure string
  # code, no hardware; player.h for NAME_MAX only. Same names.c the host
  # name-hash, resume and names tests compile. idx.c is the CORELIB.IDX header
  # validator and record CRC-32 — the part of the index loader that decides
  # whether to trust a file off the user's disk — likewise extracted from
  # main.c, where the host test had to carry verbatim copies of it. Same
  # idx.c the host idx test compiles. sort.c is the stable index-array merge
  # sort every alphabetised list is built with; same sort.c the host sort test
  # compiles against a stable reference. playlist.c is the Playlists read
  # path: the .m3u8 files under <library root>/Playlists, and one of them
  # parsed (fs/m3u.c) and resolved (fat32_resolve_path) into playable rows
  # named and located exactly as browse rows are; the record binding stays
  # in main.c. Same playlist.c the host playlist test compiles against an
  # in-RAM volume.
  library_hw_lib = static_library(
    'core_library_hw',
    sources: ['library/names.c', 'library/idx.c', 'library/sort.c',
              'library/playlist.c', 'library/fold.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Music > Search (ours): the character ring, the query edits, the linear
  # match over the library, the ranking, and the painter for an entirely new
  # screen. Almost all of it is judgement that has to be pinned — what a
  # keystroke does to the query, which of two matches comes first, what "no
  # matches" looks like — so it lives where the host can assert it. WHAT to
  # scan and how a hit reads as a row are injected from kernel_main, the same
  # seam wheel.c and chrome.c already use. Same search.c the host search_test
  # compiles; folding comes from library/fold.c in library_hw_lib.
  search_hw_lib = static_library(
    'core_search_hw',
    sources: ['ui/search.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Full-screen "charging" battery view (ours): renders design_reference's
  # ChargingScreen into the RGB565 console framebuffer with fill_rect shapes +
  # the Nunito text renderer. Pure integer rendering — no libc/libm/malloc, no
  # hardware access. Relative includes only (ui/text.h, ../kernel/console.h,
  # ../hal/hal.h), so no extra include dirs; stays under -Werror.
  screen_charging_hw_lib = static_library(
    'core_screen_charging_hw',
    sources: ['ui/screen_charging.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Low-battery warning UI (ours): the DISKSAFE modal, the SHUTOFF goodbye
  # screen, the LOW toast, and the pure battwarn_* state machine that keeps a
  # sagging-and-recovering cell from flickering them. Same primitives and
  # palette as the charging screen (it links against screen_charging_hw_lib's
  # shared battery glyph); no hardware, no present — the SAME source the host
  # screen_battery_test compiles. Relative includes only; stays under -Werror.
  screen_battery_hw_lib = static_library(
    'core_screen_battery_hw',
    sources: ['ui/screen_battery.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Freestanding incremental album-art thumbnail cache (ours): loads AT MOST one
  # album cover per pump() call and pre-shrinks it to a 22x22 list chip, spreading
  # the disk I/O across many main-loop passes so scrolling the album list never
  # stalls the audio decode/DMA loop. Uses thumb.c to downscale and fat32 to read;
  # no libc/libm/malloc, no allocation. Reaches fat32.h + thumb.h via relative
  # includes (no extra include dirs); stays under -Werror since we own it.
  artcache_hw_lib = static_library(
    'core_artcache_hw',
    sources: ['ui/artcache.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Settings subsystem (ours): a data-driven settings_t model + a matching
  # renderer for the Settings screens (design_reference menus.jsx + ThemePicker).
  # settings.c is pure model logic (no framebuffer/hardware — the SAME source the
  # host settings_test compiles); eq.c is the equaliser preset table it and
  # kernel/main.c read; screen_settings.c draws it via the Nunito text
  # renderer + console fill_rect. main.c owns the settings_t state + navigation
  # and calls these generically. settime.c is the Date & Time field editor's
  # model, pure the way settings.c is (the host settime_test compiles the SAME
  # source); its painter sits beside the other Settings painters in
  # screen_settings.c. No libc/libm/malloc; relative includes only
  # (settings.h, ui/text.h, ../kernel/console.h, ../hal/hal.h); stays -Werror.
  settings_hw_lib = static_library(
    'core_settings_hw',
    sources: ['ui/settings.c', 'ui/screen_settings.c', 'ui/palette.c',
              'ui/eq.c', 'ui/settime.c'],
    c_args: ['-DCORE_FREESTANDING'],
  )

  # Background streaming playback engine (ours): the FLAC/MP3 decode path, the
  # SPSC PCM ring feeding the DMA DAC, the fat32 byte-source + read-ahead glue,
  # album art, and the queue/auto-advance player — extracted out of kernel/main.c
  # into core/player/. Freestanding (its struct-copy lowers to lib/mem.c's memcpy,
  # resolved at the final link against mem_dep). Needs the hal include dir for
  # hw/*.h + hal.h; codec/kernel/fs headers are reached via relative includes.
  player_hw_lib = static_library(
    'core_player_hw',
    sources: ['player/player.c'],
    include_directories: include_directories('hal', 'codecs', 'codecs/dr_flac', 'codecs/pvmp3'),
  )

  core_elf = executable(
    'core.elf',
    boot_sources + kernel_sources + fs_sources + files('codecs/arena.c')
      + [core_version_h, core_version_tag_h],   # generated stamps; main.c includes both
    dependencies: [hal_dep, mem_dep],   # hal/hw: SER0 UART; mem: freestanding memcpy/…
    link_with: [flac_hw_lib, pvmp3_hw_lib, mp3_hw_lib, flac_meta_hw_lib, meta_text_hw_lib, readahead_hw_lib, diskbuf_hw_lib, text_hw_lib, chrome_hw_lib, wheel_hw_lib, letterindex_hw_lib, search_hw_lib, keyhold_hw_lib, gesture_hw_lib, sleeptimer_hw_lib, jackwatch_hw_lib, library_hw_lib, thumb_hw_lib, screen_charging_hw_lib, artcache_hw_lib, settings_hw_lib, player_hw_lib, screen_battery_hw_lib],
    include_directories: codec_inc,
    link_args: [
      '-Wl,-T,' + (meson.current_source_dir() / 'boot' / 'linker.ld'),
      '-Wl,-Map=core.map',
      # libgcc: the ARM7TDMI has no hardware divide, so integer division
      # (fat32 sector math, playback timing) needs the compiler runtime's
      # __aeabi_*divmod helpers. This is the compiler runtime, NOT libc.
      '-lgcc',
      # The pcm_ring SPSC indices use __atomic acquire/release ordering.
      # On ARMv4T (no barrier instruction) GCC lowers the fence to a
      # __sync_synchronize libcall; sync-none selects its no-op form. That
      # is correct HERE: the ring's producer (foreground pump) and consumer
      # (DMA ISR) run on the SAME core — ARM7TDMI is in-order and strongly
      # ordered, and the DMA engine reads the PCM buffer, never the indices,
      # so no cross-observer hardware fence is needed. The __atomic builtins
      # still emit the compiler-ordering barrier we actually rely on. Absent
      # this spec the stub links anyway but warns on every build.
      #
      # -specs=sync-none.specs ALONE does not take effect here: the spec body is
      # guarded by `%{!nostdlib|lgcc: --defsym=...}`, and we link with -nostdlib
      # while -lgcc is an input FILE rather than a spec-visible switch, so the
      # guard never fires and libgcc's warning-carrying _sync_synchronize.o was
      # still pulled in ("This implementation of __sync_synchronize is a stub
      # with no effect"). Passing the same --defsym the spec would have passed
      # states the intent unconditionally and stops the archive member (and its
      # .gnu.warning section) from ever being needed. The spec is kept so the
      # declaration is discoverable by the usual name.
      '-specs=sync-none.specs',
      '-Wl,--defsym=__sync_synchronize=__sync_synchronize_none',
      # Bare-metal image: .text/.data/.bss share one RWX SDRAM segment by
      # design (there is no MMU and no loader to honour page permissions), so
      # ld's hardening warning about it is noise, not a finding.
      '-Wl,--no-warn-rwx-segments',
    ],
    link_depends: boot_linker_script,
  )

  # Flat binary derived from the ELF — the host-side `core firmware pack`
  # wraps this in the .ipod transport format. Both products are byproducts
  # of `make hw`; the Makefile's `ipod` target does the .ipod wrap.
  objcopy = find_program('objcopy')
  custom_target(
    'core.bin',
    input: core_elf,
    output: 'core.bin',
    command: [objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@'],
    build_by_default: true,
  )

  # Post-link size / static-RAM budget gate. `ninja` runs this every time
  # core.elf relinks: it prints the text/data/bss breakdown and FAILS the build
  # when a section is past the budget documented in the script. Without it a new
  # MB-scale static buffer grows .bss silently — .bss is already ~10.1 MB inside
  # a 32 MB SDRAM window, and nothing else in the tree reports the number.
  # The output file is just a stamp holding the printed report.
  size_prog = find_program('arm-none-eabi-size', required: true)
  custom_target(
    'core-size-budget',
    input: core_elf,
    output: 'core.size',
    command: [
      find_program('sh'),
      meson.current_source_dir() / 'tests' / 'scripts' / 'check_size.sh',
      '@INPUT@', size_prog.full_path(), '@OUTPUT@',
    ],
    build_by_default: true,
  )

elif target == 'sim'
  subdir('codecs')      # vendored audio decoders (FLAC + MP3)
  subdir('tests')       # host unit tests for the freestanding firmware code
endif

# Future subsystems land here as they're written:
#
#   target == 'hw':  fs/ (FAT32), then codecs/apps once freestanding-clean
#   either:          kernel/ scheduler, IRQ table
