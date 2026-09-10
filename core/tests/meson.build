# Test harness: codec known-answer tests.
#
# Builds codec_kat as a host executable; the fixture directory is
# passed at runtime. `meson test` picks them up.

if target == 'sim'
  # libm needed only on platforms where it's a separate library;
  # macOS and most BSDs link math via libSystem and reject -lm.
  cc = meson.get_compiler('c')
  m_dep = cc.find_library('m', required: false)

  # TEST BUILD OPTIONS — deliberately NOT the firmware's.
  #
  # The project defaults are tuned for the thing that ships on a 32 MB
  # ARM7TDMI: -Os with LTO. Those are the wrong defaults for a test binary.
  # -Os inlines and reorders until a crash's stack trace names the wrong
  # function, and LTO can optimize away exactly the undefined behaviour a
  # sanitizer build exists to catch (a dead store past the end of an array
  # simply disappears). Tests want to be readable under a debugger and honest
  # under ASan/UBSan, and they are never size-constrained.
  #
  # -O1 rather than -O0: still fully debuggable, but keeps the codec KATs and
  # the 60-pick shuffle loops fast enough that nobody is tempted to skip the
  # suite. Applied per-target via override_options so the vendored codec
  # libraries the tests link against keep the firmware's own settings.
  test_opts = ['optimization=1', 'b_lto=false']

  codec_kat = executable(
    'codec_kat',
    sources: ['codec_kat.c'],
    dependencies: [codecs_dep, m_dep],
    override_options: test_opts,
  )

  vectors_dir = meson.current_source_dir() / 'codec-vectors'
  # codec_kat runs every vendored codec's KAT in one binary; one
  # meson test entry covers all of them. Failures print which codec
  # failed via the [codec] label in stderr.
  test('codec-kat', codec_kat,
       args: [vectors_dir],
       suite: 'unit')

  # flac-freestanding: the STREAMING FLAC decode path used on hw — open a
  # FLAC through flac_open_stream (a decoder_source_t pull source) with the
  # static arena instead of libc malloc, decode every frame, bit-compare to
  # the reference PCM, and assert the arena never OOM'd. Same flac.c + arena.c
  # the ARM build compiles; only the byte source differs (memory vs disk).
  flac_fs_test = executable(
    'flac_freestanding_test',
    sources: ['codecs/flac_freestanding_test.c', '../codecs/arena.c'],
    dependencies: [flac_dep],
    include_directories: include_directories('../codecs', '../codecs/dr_flac'),
    override_options: test_opts,
  )
  test('flac-freestanding', flac_fs_test, args: [vectors_dir], suite: 'unit')

  # mp3-freestanding: the STREAMING MP3 decode path used on hw — mirror of
  # flac-freestanding. Open the MP3 through mp3_open_stream (a decoder_source_t
  # pull source) with the static arena instead of libc malloc, decode every
  # frame, bit-compare to the reference PCM (dr_mp3's own captured output, the
  # KAT truth for a lossy codec), and assert the arena never OOM'd. Same mp3.c +
  # arena.c the ARM build compiles; only the byte source differs (memory vs disk).
  mp3_fs_test = executable(
    'mp3_freestanding_test',
    sources: ['codecs/mp3_freestanding_test.c', '../codecs/arena.c'],
    dependencies: [mp3_dep],
    include_directories: include_directories('../codecs', '../codecs/dr_mp3'),
    override_options: test_opts,
  )
  test('mp3-freestanding', mp3_fs_test, args: [vectors_dir], suite: 'unit')

  # readahead: the block-buffering shim (codecs/readahead.c) that collapses a
  # decoder's many tiny reads into a few large backing reads — the fix for the
  # ~27 s on-device MP3 startup. Backed by an instrumented in-RAM source that
  # counts backing reads, so the test proves both byte-exactness and coalescing.
  # Same readahead.c the ARM build compiles.
  readahead_test = executable(
    'readahead_test',
    sources: ['codecs/readahead_test.c', '../codecs/readahead.c'],
    include_directories: include_directories('../codecs'),
    override_options: test_opts,
  )
  test('readahead', readahead_test, suite: 'unit')

  # flac-meta: the FLAC metadata reader (codecs/flac_meta.c) — the SAME source
  # the ARM build links into core.elf. Backs a decoder_source_t with an in-RAM
  # buffer and asserts: the committed KAT vector parses to sample_rate 44100 /
  # 1 s duration (its VORBIS_COMMENT has no tags), a synthetic FLAC we build in
  # RAM round-trips a full tag set (title/artist/ALBUMARTIST fallback/album/
  # genre/"7/12" track/"2021-05-01" date) with UTF-8 stripped, and malformed /
  # hostile inputs return -1 or bail without overrunning. Takes the fixture dir
  # as argv[1] to find the KAT flac.
  flac_meta_test = executable(
    'flac_meta_test',
    sources: ['codecs/flac_meta_test.c', '../codecs/flac_meta.c'],
    include_directories: include_directories('../codecs'),
    override_options: test_opts,
  )
  test('flac-meta', flac_meta_test, args: [vectors_dir], suite: 'unit')

  # diskbuf: the MB-scale anti-skip buffer (codecs/diskbuf.c) that reads ahead in
  # bursts so the drive head parks between refills (the "constant arm movement"
  # fix). Backed by the same instrumented in-RAM source; asserts byte-exactness
  # across reads/seeks AND that the pump is bursty (fills to high, idles, refills
  # below low). Same diskbuf.c the ARM build compiles.
  diskbuf_test = executable(
    'diskbuf_test',
    sources: ['codecs/diskbuf_test.c', '../codecs/diskbuf.c'],
    include_directories: include_directories('../codecs'),
    override_options: test_opts,
  )
  test('diskbuf', diskbuf_test, suite: 'unit')

  # text: the freestanding antialiased text renderer (core/ui/text.c) — the
  # SAME source the ARM build links into core.elf. Renders "Ag" into a small
  # RGB565 buffer over a known bg/ink and asserts width is positive+additive
  # and matches the draw advance, that interior pixels blended toward the ink
  # (incl. partial-coverage greys, proving the gamma blend ran), that pixels
  # outside coverage are untouched, and that drawing off every edge writes no
  # OOB (sentinel guard cells bracket the framebuffer). No MMIO, plain cc.
  text_test = executable(
    'text_test',
    sources: ['ui/text_test.c', '../ui/text.c'],
    include_directories: include_directories('../ui'),
    override_options: test_opts,
  )
  test('text', text_test, suite: 'unit')

  # chrome: the shared list/panel chrome (core/ui/chrome.c) — the SAME source
  # the ARM build links into core.elf. This target is the reason chrome.c
  # exists: every function in it was a `static` inside kernel/main.c, which
  # meson builds only for target == 'hw', so none of the list geometry,
  # selection bars, scroll clamps or header layout could be checked without
  # flashing the device. Draws into console.c's back buffer and measures glyphs
  # through text.c; the marquee's clock is injected and left unset here, so
  # overflowing titles clip. Assertions read pixels back out of the
  # framebuffer rather than trusting return values.
  chrome_test = executable(
    'chrome_test',
    sources: ['ui/chrome_test.c', '../ui/chrome.c', '../ui/text.c',
              '../ui/palette.c', '../kernel/console.c'],
    include_directories: include_directories('../ui', '../kernel', '../hal'),
    override_options: test_opts,
  )
  test('chrome', chrome_test, suite: 'unit')

  # screen_battery: the low-battery warning UI (core/ui/screen_battery.c) — the
  # SAME source the ARM build links into core.elf. The renders are checked
  # against the framebuffer (full-panel coverage, red kept inside the glyph, the
  # toast touching nothing outside its plate and reporting exactly that plate as
  # damage), and the pure battwarn_* state machine is driven through the whole
  # flap-prevention sequence with the clock straddling the 32-bit timer wrap.
  # Pulls in screen_charging.c for the shared battery glyph + palette, and
  # hw/battery.h (header only, no MMIO) to static-assert the level contract.
  screen_battery_test = executable(
    'screen_battery_test',
    sources: ['ui/screen_battery_test.c', '../ui/screen_battery.c',
              '../ui/screen_charging.c', '../ui/chrome.c', '../ui/text.c',
              '../ui/palette.c', '../kernel/console.c'],
    include_directories: include_directories('../ui', '../kernel', '../hal'),
    override_options: test_opts,
  )
  test('screen_battery', screen_battery_test, suite: 'unit')

  # wheel: the wheel acceleration state machine and A-Z locator
  # (core/ui/wheel.c) — the SAME source the ARM build links into core.elf.
  # Its clock, row-initial source and click are injected, so every event here
  # is an exact (delta, gap) pair and the assertions are the exact selection
  # indices a spin lands on: the detent arithmetic (never 1,1,2), the flick
  # cap, the clamps, the velocity curve event by event (1,2,5,7,8), the
  # letter-mode latch surviving a slowdown and clearing on the idle gap, the
  # letterless-screen fall-through, letter stepping at both ends, the plate's
  # hold window to the microsecond, and a gap across the 32-bit clock wrap.
  wheel_test = executable(
    'wheel_test',
    sources: ['ui/wheel_test.c', '../ui/wheel.c'],
    include_directories: include_directories('../ui', '.'),
    override_options: test_opts,
  )
  test('wheel', wheel_test, suite: 'unit')

  # thumb: the freestanding nearest-neighbor RGB565 downscaler (core/ui/thumb.c)
  # — the SAME source the ARM build links into core.elf. Shrinks a loaded
  # folder.art to a list chip / album-detail cover when no folder.thm is present.
  # Asserts an exact identity copy (dst == src dims), a hand-worked 4x4->2x2 pixel
  # selection, in-bounds source reads + no OOB writes for a non-integer ratio
  # (120->22, sentinel-guarded dst), and the zero/negative-dim no-op. Plain cc.
  thumb_test = executable(
    'thumb_test',
    sources: ['ui/thumb_test.c', '../ui/thumb.c'],
    include_directories: include_directories('../ui'),
    override_options: test_opts,
  )
  test('thumb', thumb_test, suite: 'unit')

  # artcache: the album-cover cache (core/ui/artcache.c) — the SAME source the
  # ARM build links into core.elf — driven the way the album list drives it.
  # fat32_read_file is stubbed by the test (it is artcache.c's only FS entry
  # point), so every cache miss is a counted, ordered "disk read": the test
  # measures what a scroll gesture costs in reads and which covers load first,
  # which on the device are the reads issued synchronously under the wheel.
  artcache_test = executable(
    'artcache_test',
    sources: ['ui/artcache_test.c', '../ui/artcache.c', '../ui/thumb.c'],
    include_directories: include_directories('../ui', '../fs'),
    override_options: test_opts,
  )
  test('artcache', artcache_test, suite: 'unit')

  # settings: the Settings state model (core/ui/settings.c) — the SAME source the
  # ARM build links into core.elf. Pure model logic (no framebuffer/hardware), so
  # it compiles with plain cc. Asserts defaults, that activate() toggles Shuffle
  # and cycles Repeat OFF->ALL->ONE->OFF, that adjust() clamps Volume/Bass and
  # Display Brightness to range, that Backlight cycles its discrete list, and that
  # Root rows return the right ENTER_*/RESET action codes plus value/kind report.
  settings_test = executable(
    'settings_test',
    sources: ['ui/settings_test.c', '../ui/settings.c'],
    include_directories: include_directories('../ui'),
    override_options: test_opts,
  )
  test('settings', settings_test, suite: 'unit')

  tag_mp3_test = executable(
    'tag_mp3_test',
    sources: ['tag_mp3_test.c'],
    dependencies: [codecs_dep],
    override_options: test_opts,
  )
  test('tag-mp3', tag_mp3_test, suite: 'unit')

  # hw-mmio golden-trace tests: compile the freestanding hw drivers
  # host-side with -DMMIO_MOCK so the recording fake bus
  # (hw_mmio/mmio_mock.c) captures the exact register-access grammar,
  # then assert it. This is the only automated check of the BCM/LCD
  # transaction stream — clicky models no BCM, the device needs a logic
  # analyzer. The drivers compile unchanged (freestanding: stdint +
  # hal.h + the mmio.h seam only); in the firmware build the same seam
  # is a static-inline volatile deref, so core.bin is byte-identical.
  hw_mmio_inc = include_directories('../hal/hw', '../hal', 'hw_mmio')

  uart_trace_test = executable(
    'uart_trace_test',
    sources: ['hw_mmio/uart_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/uart.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-uart-trace', uart_trace_test, suite: 'unit')

  lcd_trace_test = executable(
    'lcd_trace_test',
    sources: ['hw_mmio/lcd_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/lcd.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-lcd-trace', lcd_trace_test, suite: 'unit')

  # hw-bcm-init: the BCM bootstrap + firmware upload (bcm_init/lcd_recover in
  # hal/hw/lcd.c) against the recording mock bus. This is a RECOVERY path that
  # nothing on the normal boot path calls, and it can never run on the device
  # without risking the only debug channel the device has — so this golden
  # trace, asserted step for step against docs/hw/02-lcd.md "Bootstrap and
  # firmware upload", is the only check it will ever get before hardware.
  # Built with -DLCD_RECOVER_ON_WAKE=1 so the post-wake wiring is exercised
  # too; the shipping image leaves that flag at its default of 0.
  bcm_init_test = executable(
    'bcm_init_test',
    sources: ['hw_mmio/bcm_init_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/lcd.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK', '-DLCD_RECOVER_ON_WAKE=1'],
    override_options: test_opts,
  )
  test('hw-bcm-init', bcm_init_test, suite: 'unit')

  # Static header<->doc consistency: pp5022.h addresses vs the
  # docs/hw/ reference + internal stride/window invariants. Pure source
  # parsing (no ARM build needed), so it runs in the common test path.
  # Catches the dropped-zero and byte-vs-word-stride bug classes. The
  # ELF-layout half (crt0/linker asserts, needs core.elf) runs via
  # `make verify-hw`.
  py = find_program('python3', required: false)
  if py.found()
    test('hw-doc-consistency', py,
         args: [meson.current_source_dir() / 'scripts' / 'check_hw_consistency.py'],
         suite: 'unit')
  endif

  # hw-sched: the cooperative scheduler's task-table + round-robin
  # policy, compiled host-side with -DSCHED_TEST against inert stubs for
  # the two asm symbols (switch_context / task_trampoline). Proves task
  # add/bounds, round-robin selection, done-task skipping, and the
  # single-runnable no-switch case. The context-switch asm itself has NO
  # automated coverage: it has only ever been run by hand in the clicky
  # emulator, which is not wired up as a target in this repo.
  sched_test = executable(
    'sched_test',
    sources: ['hw_sched/sched_test.c',
              'hw_sched/sched_test_mocks.c',
              '../kernel/sched.c'],
    include_directories: include_directories('../kernel'),
    c_args: ['-DSCHED_TEST'],
    override_options: test_opts,
  )
  test('hw-sched', sched_test, suite: 'unit')

  # hw-timer: the interrupt dispatcher + 100 Hz timer tick, compiled
  # host-side with -DMMIO_MOCK against the recording fake bus. Asserts
  # timer_init's exact register grammar (arm CFG + unmask), that
  # timer_tick_isr acks by reading TIMER1_VAL and advances the counter,
  # that irq_dispatch only ticks when CPU_INT_STAT shows TIMER1 pending,
  # and the sleep_ms tick math (a sched_yield stub advances the mocked
  # tick so the cooperative wait terminates). The IRQ entry asm + real
  # interrupt delivery have NO automated coverage — manual clicky runs only,
  # and there is no clicky target in this repo.
  timer_test = executable(
    'timer_test',
    sources: ['hw_timer/timer_test.c',
              'hw_timer/timer_test_stubs.c',
              '../kernel/timer.c',
              '../kernel/irq.c',
              'hw_mmio/mmio_mock.c'],
    include_directories: include_directories('../hal', '../hal/hw', '../kernel', 'hw_mmio'),
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-timer', timer_test, suite: 'unit')

  # hw-clock: the PLL/clock-boost driver's register grammar (arm CFG +
  # lock poll + timing), the bounded lock-spin (clicky never sets the PLL
  # lock bit), and the boost refcount edges — host-side under -DMMIO_MOCK.
  clock_test = executable(
    'clock_test',
    sources: ['hw_clock/clock_test.c',
              '../kernel/clock.c',
              'hw_mmio/mmio_mock.c'],
    include_directories: include_directories('../hal', '../hal/hw', '../kernel', 'hw_mmio'),
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-clock', clock_test, suite: 'unit')

  # hw-audio: the first-sound driver chain (I2C controller -> WM8758
  # codec -> I2S serializer), host-side under -DMMIO_MOCK. The only
  # automated check of the audio register grammar — clicky models no
  # I2C/I2S/DAC and the device needs a logic analyzer; sound itself is
  # the on-device proof. Asserts i2c_init/i2c_send grammar + guards, the
  # 30-write wm8758_init with correct 9-bit framing / DAC route / unmute-
  # last ordering, i2s_init clock+FIFO grammar, and the polled write
  # (TXFree poll + packed word, plus the no-drain bail).
  audio_trace_test = executable(
    'audio_trace_test',
    sources: ['hw_audio/audio_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/i2c.c',
              '../hal/hw/wm8758.c',
              '../hal/hw/i2s.c',
              '../hal/hw/dma.c'],
              # No ata.c: the two ATA cases that were misfiled in this suite
              # moved to hw_ata. Linking a driver a test does not exercise
              # invites exactly that kind of drift back.
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-audio', audio_trace_test, suite: 'unit')

  # fat32: the read-only FAT32 reader, host-tested against a synthetic
  # image (BytesPerSector 2048 like the stock iPod 80 GB drive, plus a
  # file spanning two clusters) generated by make_fat32_image.py. Portable
  # logic, no MMIO — plain cc. Verifies mount, the 8.3 lookup (hit/miss,
  # case-insensitive), and full + partial file reads.
  if py.found()
    fat32_img = custom_target('fat32-test-image',
      output: 'fat32_test.img',
      command: [py, files('scripts' / 'make_fat32_image.py'), '@OUTPUT@'])
    fat32_test = executable('fat32_test',
      sources: ['fs/fat32_test.c', '../fs/fat32.c'],
      include_directories: include_directories('../fs'),
      override_options: test_opts)
    test('fat32', fat32_test, args: [fat32_img], suite: 'unit')
  endif

  # m3u: the M3U8 playlist reader. No image and no python needed — the parser
  # takes a byte-source callback precisely so a malformed playlist is a string
  # literal in the test rather than a synthetic FAT32 volume, and the corpus
  # here is mostly malformed on purpose (BOM, CRLF, backslashes, "..", control
  # bytes, over-long lines, more tracks than the caller's array, a source that
  # never ends). It links fat32.c as well because the last section drives the
  # on-disk adapter m3u_parse_file() against a hand-built in-RAM volume.
  #
  # The endless-source case would HANG rather than fail if the byte cap
  # regressed, so this test carries meson's default timeout deliberately.
  m3u_test = executable('m3u_test',
    sources: ['fs/m3u_test.c', '../fs/m3u.c', '../fs/fat32.c'],
    include_directories: include_directories('../fs'),
    override_options: test_opts)
  test('m3u', m3u_test, suite: 'unit')

  # pcm-ring: the SPSC PCM ring that decouples the streaming disk pump
  # (producer) from the DMA-completion ISR (consumer). Portable logic, no
  # MMIO — verifies free/fill accounting, over-write / under-read clamping,
  # the power-of-two wrap, and end-to-end FIFO byte integrity across many
  # wrap cycles.
  pcm_ring_test = executable(
    'pcm_ring_test',
    sources: ['kernel/pcm_ring_test.c', '../kernel/pcm_ring.c'],
    include_directories: include_directories('../kernel'),
    override_options: test_opts,
  )
  test('pcm-ring', pcm_ring_test, suite: 'unit')

  # config: settings persistence (kernel/config.c) — the ONLY code in the
  # firmware that writes to the user's disk, so this is the highest-stakes
  # test in the suite. Four layers over an in-RAM FAT32 volume with
  # BytesPerSector 2048 at a non-zero partition base (the geometry that makes
  # the right and wrong LBA formulas differ):
  #   1. fat32_file_lba pins the absolute LBA to
  #      part_lba + cluster_fs_sector * sec_ratio, and explicitly asserts it
  #      is NOT the sec_ratio-less value the design doc proposed — which on
  #      the stock volume lands inside the FAT.
  #   2. the record codec: round-trip, and rejection of bad magic / version /
  #      length / a flipped payload, header or padding bit / all-zeros /
  #      garbage, plus range clamping of a CRC-valid but insane record.
  #   3. two-slot selection through config_load: newer seq wins, a torn or
  #      garbage slot loses to a good one, seq WRAPAROUND resolves correctly,
  #      both-bad keeps defaults, absent/short file disables writing.
  #   4. the write grammar, host-side under -DMMIO_MOCK: the exact
  #      WRITE SECTORS 0x30 / task-file / DRQ-out / FLUSH CACHE 0xE7 sequence
  #      ata.c emits, at the resolved LBA, with slot ALTERNATION asserted —
  #      plus that a refused save emits ZERO bus events.
  # Links the real ata.c, so the trace is the driver's, not a re-description
  # of it. What it cannot prove is that the drive obeys: see the on-device
  # bring-up procedure at the top of kernel/config.c.
  config_test = executable(
    'config_test',
    sources: ['kernel/config_test.c',
              '../kernel/config.c',
              '../ui/settings.c',      # for settings_defaults(): the host tool's
                                       # record must agree with it field-for-field
              '../fs/fat32.c',
              '../hal/hw/ata.c',
              'hw_mmio/mmio_mock.c'],
    include_directories: [hw_mmio_inc,
                          include_directories('../kernel', '../fs', '../ui')],
    c_args: ['-DMMIO_MOCK'],
    # This one was missing test_opts, so the suite's highest-stakes binary —
    # the only check of the code that WRITES to the user's disk — was the one
    # built at -Os with LTO, exactly the settings the comment at the top of
    # this file explains are wrong for a test.
    override_options: test_opts,
  )
  if py.found()
    # 5. Host/device format agreement: the fixture is produced by the REAL
    # host tool (tools/make_config.py --emit), and the test decodes it with
    # the firmware's own config_decode(). The two encoders are independent
    # implementations of one spec — offsets, field order, signedness, CRC
    # polynomial and coverage — so without this they can drift and a freshly
    # created CORECFG.DAT would be silently rejected on device, with the only
    # symptom being "settings never persist" on a user's iPod.
    corecfg_fixture = custom_target('corecfg-fixture',
      output: 'corecfg_golden.bin',
      command: [py, files('..' / '..' / 'tools' / 'make_config.py'),
                '--emit', '@OUTPUT@'])
    test('config', config_test, args: [corecfg_fixture], suite: 'unit')
  else
    test('config', config_test, suite: 'unit')
  endif

  # hw-console: the on-screen debug console (framebuffer + 8x8 hex font).
  # Pure rendering logic, no MMIO — verifies glyph output pixel-for-pixel.
  console_test = executable(
    'console_test',
    sources: ['hw_console/console_test.c', '../kernel/console.c'],
    include_directories: include_directories('../kernel', '../hal'),
    override_options: test_opts,
  )
  test('hw-console', console_test, suite: 'unit')

  # hw-clickwheel: the click-wheel/button/hold decode, host-side under
  # -DMMIO_MOCK. The status-word decode is pure integer math, so the mock
  # bus feeds synthetic 32-bit words (CLICKWHEEL_DATA) and a scripted hold
  # GPIO into the real clickwheel_poll() and asserts button decode, the
  # sensitivity-gated wheel delta (incl. sub-threshold accumulation and
  # wrap through 0x5F<->0x00), the valid-gate rejection, and the hold edge.
  clickwheel_test = executable(
    'clickwheel_test',
    sources: ['hw_clickwheel/clickwheel_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/clickwheel.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-clickwheel', clickwheel_test, suite: 'unit')

  # hw-lcd-present: lcd_present() streams a caller framebuffer over the
  # BCM (same path as lcd_fill) — asserts the exact 38400-word grammar
  # and pixel packing against the recording mock bus.
  lcd_present_test = executable(
    'lcd_present_test',
    sources: ['hw_mmio/lcd_present_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/lcd.c'],
    include_directories: include_directories('../hal', '../hal/hw', 'hw_mmio'),
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-lcd-present', lcd_present_test, suite: 'unit')

  # hw-backlight: the GPIO-bit-banged backlight dimmer (hal/hw/backlight.c)
  # host-side under -DMMIO_MOCK. The backlight has no status register — it's
  # a bare GPIO charge-pump dimmer — so this asserts the exact ordered write
  # grammar of backlight_init (route + power + 16->32 walk + LED on),
  # backlight_set(0) (single atomic LED-off), and backlight_set(MAX) (LED-on
  # + walk to full), each op a single write to the +0x800 atomic set/clear
  # alias. Pulse widths are untraced busy-loops (device-gated timing).
  backlight_trace_test = executable(
    'backlight_trace_test',
    sources: ['hw_mmio/backlight_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/backlight.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-backlight', backlight_trace_test, suite: 'unit')

  # hw-battery: the battery gauge (PCF50605 PMU ADC over I2C) + power-state
  # (GPIO input bits), host-side under -DMMIO_MOCK. There is no clicky PMU
  # model and the device needs a logic analyzer, so this is the only
  # automated check of the battery read grammar. It exercises the new
  # i2c.c register-pointer READ path end-to-end: asserts battery_millivolts
  # emits the exact channel-select write (ADCC1=0x05) + 2-byte result read
  # (ADCS1/ADCS2) transaction stream and returns the seeded raw's mV, and
  # that power_is_external/charging read the right GPIO input reg/width and
  # decode the right bit + polarity. Scaling + %-curve are DEVICE-GATED
  # (calibrate against the real cell); the test pins the transaction shape,
  # not the on-cell accuracy.
  battery_trace_test = executable(
    'battery_trace_test',
    sources: ['hw_mmio/battery_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/battery.c',
              '../hal/hw/i2c.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-battery', battery_trace_test, suite: 'unit')

  # hw-headphone-*: the headphone-jack insertion-detect driver
  # (hal/hw/headphone.c) — the SAME source the ARM build links — host-side
  # under -DMMIO_MOCK. The driver has three compile-time shapes and each
  # carries a promise the device cannot cheaply check, so one source builds
  # three binaries (headphone_trace_test.c picks its main() by the macros):
  #
  #   trace      (-DHEADPHONE_DETECT_TRUSTED=1): one poll is exactly one
  #              USEC_TIMER read + one GPIOA_INPUT_VAL read; bit 7 decodes with
  #              the documented polarity; and the DEBOUNCE debounces — a
  #              bouncing switch yields exactly one transition, timed from the
  #              last bounce; sub-window glitches yield none; the first sample
  #              primes without waiting; a USEC_TIMER wrap mid-window is fine.
  #   untrusted  (defaults, what ships until the device transcript confirms
  #              the line): answers -1 and never touches the bus.
  #   probe      (-DHEADPHONE_PROBE=1): the pin-finding UART probe prints its
  #              config block once, nothing while inputs are static, two lines
  #              per change, honours its 20 ms sample interval, and goes silent
  #              after its event budget — it cannot flood the debug channel.
  #              Links uart.c so the bytes are counted at SER0_THR.
  headphone_trace_test = executable(
    'headphone_trace_test',
    sources: ['hw_mmio/headphone_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/headphone.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK', '-DHEADPHONE_DETECT_TRUSTED=1'],
    override_options: test_opts,
  )
  test('hw-headphone-trace', headphone_trace_test, suite: 'unit')

  headphone_untrusted_test = executable(
    'headphone_untrusted_test',
    sources: ['hw_mmio/headphone_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/headphone.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-headphone-untrusted', headphone_untrusted_test, suite: 'unit')

  headphone_probe_test = executable(
    'headphone_probe_test',
    sources: ['hw_mmio/headphone_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/headphone.c',
              '../hal/hw/uart.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK', '-DHEADPHONE_DETECT_TRUSTED=1',
             '-DHEADPHONE_PROBE=1'],
    override_options: test_opts,
  )
  test('hw-headphone-probe', headphone_probe_test, suite: 'unit')

  # hw-volume: the WM8758 output-volume HAL (hal/hw/volume.c) over the
  # reused i2c.c write path, host-side under -DMMIO_MOCK. Volume is not
  # modelled by clicky and the device needs a logic analyzer, so this is
  # the automated check of the percent->register policy: the PURE
  # percent->OUT1VOL mapping (monotonic gain, clamping, 0%=mute,
  # 100%=0 dB/0x39 never +6 dB) plus hal_volume_set's L-then-R(+VU)
  # two-write codec grammar and hal_volume_get round-trip.
  volume_trace_test = executable(
    'volume_trace_test',
    sources: ['hw_mmio/volume_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/volume.c',
              '../hal/hw/i2c.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-volume', volume_trace_test, suite: 'unit')

  # ------------------------------------------------------------------
  # Tests added by the build/test hardening pass. Each covers a surface
  # that previously had none; several assert the DOCUMENTED contract and
  # mark the gaps against today's code as expected failures (tests/xfail.h)
  # rather than being weakened until they pass. Run the suite with
  # CORE_TEST_STRICT_XFAIL=1 to see whether those gaps have been closed.
  # ------------------------------------------------------------------

  # power-standby: the single most dangerous write in the firmware — the
  # PCF50605 GOSTDBY latch. Getting OOCC1 wrong (wake bits dropped, wrong
  # register, wrong device) leaves an iPod that cannot be powered on again
  # without disassembly. battery.c and volume.c, which can at worst misreport
  # a percentage, both had trace tests over this same i2c_send path; this one
  # had none. Asserts the exact transaction, plus the invariants restated
  # independently of event order so a bounded-retry rewrite doesn't break it.
  # power_standby() deliberately never returns, so the test escapes via the
  # mock bus's post-access hook (and a watchdog for the path that strobes
  # nothing at all).
  power_trace_test = executable(
    'power_trace_test',
    sources: ['hw_mmio/power_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/power.c',
              '../hal/hw/i2c.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-power-standby', power_trace_test, suite: 'unit')

  # hw-ata: the PIO ATA driver (hal/hw/ata.c) — the code every music byte,
  # FAT sector and settings save goes through — host-side under -DMMIO_MOCK.
  # Its only host coverage used to be two happy-path cases misfiled in
  # hw-audio (init with RDY pre-set, one read with RDY|DRQ pre-set so no poll
  # ever spun). Those moved here, joined by what was missing: both bounded
  # polls proven to spin and to give up after exactly their budget; the timed
  # DRQ deadline; the ERR path with the recovery grammar (ERROR latched before
  # the reset, residual DRQ drained, soft reset) that stops a retry returning
  # shifted data as success; the >256-sector command split; standby/wakeup
  # and the parked flag; the write+flush grammar with every data word
  # checked and its failure paths routed through the same recovery; and the
  # LBA28 guard. clicky models no drive, so the device is the only other
  # place any of this runs.
  ata_trace_test = executable(
    'ata_trace_test',
    sources: ['hw_ata/ata_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/ata.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-ata', ata_trace_test, suite: 'unit')

  # hw-audio-pingpong: the ping-pong PCM buffer state machine in
  # hal/hw/audio.c. audio_trace_test asserts the register grammar of the
  # driver chain but never calls hal_audio_start / audio_dma_isr /
  # hal_audio_stop, so the code that decides which bytes reach the DAC was
  # untested. Registers a counting source and drives synthetic completions:
  # asserts the two buffers strictly alternate, that audio is neither
  # repeated nor dropped across completions, that a short source read
  # zero-pads exactly the tail, and (as a documented expected failure) that
  # stop-then-start resumes where it left off. Written against hal/hal.h's
  # contract, not the buffer size — that is a tuning knob and the test
  # discovers it at runtime.
  audio_pingpong_test = executable(
    'audio_pingpong_test',
    sources: ['hw_audio/audio_pingpong_test.c',
              'hw_audio/audio_test_stubs.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/audio.c',
              '../hal/hw/i2c.c',
              '../hal/hw/wm8758.c',
              '../hal/hw/i2s.c',
              '../hal/hw/dma.c'],
    include_directories: [hw_mmio_inc, include_directories('../kernel', '.')],
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-audio-pingpong', audio_pingpong_test, suite: 'unit')

  # names: the library's name handling (core/library/names.c) — the SAME
  # source the ARM build links into core.elf. Every function in it was a
  # `static` in kernel/main.c, which meson builds only for target == 'hw', so
  # the display-name copy every row and queue entry is made by, the
  # "Artist - Album" split, the extension classifier and the Now Playing
  # formatters had never been compiled by the host. Sentinel-guarded buffers
  # prove the NAME_MAX bounds; the UTF-8 decoder under the locator hash is
  # driven through overlongs, surrogates and truncation; one XFAIL records
  # that classify_ext matches any extension that merely STARTS with "flac".
  names_test = executable(
    'names_test',
    sources: ['library/names_test.c', '../library/names.c'],
    include_directories: include_directories('.'),
    override_options: test_opts,
  )
  test('names', names_test, suite: 'unit')

  # name-hash: the library locator hash. It exists twice — in
  # library/names.c (device) and in tools/build_index.py (host) — and it is
  # the ONLY thing binding a CORELIB.IDX record to its file on disk. If the
  # two disagree the track does not error, it silently stops resolving.
  # Compiles the real names.c and asserts a golden vector table covering
  # ASCII/case, Latin-1, smart quotes, dashes, punctuation, CJK and astral
  # codepoints. The Python half runs in hw-name-hash-parity below.
  name_hash_test = executable(
    'name_hash_test',
    sources: ['kernel/name_hash_test.c', '../library/names.c'],
    include_directories: include_directories('kernel', '.'),
    override_options: test_opts,
  )
  test('name-hash', name_hash_test, suite: 'unit')

  # resume: which library song the boot path may re-open for a saved resume
  # position (kernel/main.c resume_find_song). The rest of the resume feature
  # degrades gracefully — a missed capture loses a position, a rejected record
  # means "start from the menu" — but this is the one answer that is ACTED ON,
  # so a wrong one wakes the device playing somebody else's track. Covers the
  # happy path, every stale case (deleted / unindexed / unresolved / empty
  # library / empty locator), and the ambiguity rules: duplicate filenames are
  # disambiguated by the saved track LENGTH, and decline outright when the
  # length cannot choose. Compiles a verbatim copy of the static function;
  # hw-resume-parity below diffs it against main.c. The locator hash it
  # calls is the real library/names.c.
  resume_test = executable(
    'resume_test',
    sources: ['kernel/resume_test.c', '../library/names.c'],
    include_directories: include_directories('kernel', '.'),
    override_options: test_opts,
  )
  test('resume', resume_test, suite: 'unit')

  # idx: the CORELIB.IDX header validation and record CRC (core/library/idx.c)
  # — the SAME source the ARM build links into core.elf. The loader used to
  # read the magic and the record size and trust the rest — the version bytes
  # were never looked at and nothing checked that the file held the records it
  # claimed, so a half-copied index or a future layout loaded as plausible
  # garbage. Covers the CRC against its published check value and
  # batch-splitting, every accepted header shape (v1 without CRC, v2 with),
  # and each refusal: magic, unknown version, record size, a truncated or
  # padded file, a count that wraps the size arithmetic. hw-index-loader below
  # checks that main.c's loader actually calls these.
  idx_test = executable(
    'idx_test',
    sources: ['library/idx_test.c', '../library/idx.c'],
    include_directories: include_directories('.'),
    override_options: test_opts,
  )
  test('idx', idx_test, suite: 'unit')

  # sort: the stable index-array merge sort (core/library/sort.c) every
  # alphabetised list is built with — the SAME source the ARM build links.
  # main.c's only evidence for it was a comment saying it had been checked
  # against a reference once. Compares against a stable insertion sort over
  # the same keys — results must be IDENTICAL, so tie order is asserted, not
  # just ordering — over many sizes (non-powers of two leave a short final
  # run at every width), tie densities from all-equal to all-distinct, and
  # the full LIB_MAX_SONGS; sentinels bracket both arrays; the compare
  # counts out-of-range indices and the O(n log n) bound.
  sort_test = executable(
    'sort_test',
    sources: ['library/sort_test.c', '../library/sort.c'],
    include_directories: include_directories('.'),
    override_options: test_opts,
  )
  test('sort', sort_test, suite: 'unit')

  # player-queue: the queue / auto-advance / repeat / shuffle logic of
  # core/player/player.c — 753 lines with no test at all, and the part users
  # notice when it is wrong (a queue that stops a track early, a Repeat All
  # that doesn't wrap, a Shuffle that replays what just played, a broken file
  # that stalls instead of being skipped). The REAL player.c is compiled
  # unmodified; its disk, codec and DAC are the controllable fakes in
  # player_test_stubs.c, and the real pcm_ring.c is linked because the
  # player's use of the ring is part of what is under test. Which entry the
  # player chose is read back from the cluster the decoder was opened on, so
  # the assertions are about the file that would actually play.
  player_queue_test = executable(
    'player_queue_test',
    sources: ['player/player_queue_test.c',
              'player/player_test_stubs.c',
              'hw_mmio/mmio_mock.c',
              '../player/player.c',
              '../kernel/pcm_ring.c'],
    include_directories: include_directories(
      '../player', '../hal', '../hal/hw', '../fs', '../kernel',
      '../codecs', '../codecs/dr_flac', '../codecs/dr_mp3',
      'hw_mmio', 'player', '.'),
    c_args: ['-DMMIO_MOCK', '-DCORE_FREESTANDING'],
    override_options: test_opts,
  )
  test('player-queue', player_queue_test, suite: 'unit')

  # player-clock: the reported position against what can be heard. The DMA
  # feeder pulls whole ping-pong buffers out of the PCM ring, two of which are
  # in flight inside the HAL at any time, so "consumed from the ring" runs
  # 186..372 ms ahead of the DAC. The gapless hand-over used to present the
  # next track (and re-anchor its clock) on the consumed count. Same fakes as
  # player-queue; stub_drain() IS the feeder, so pulls are counted exactly.
  player_clock_test = executable(
    'player_clock_test',
    sources: ['player/player_clock_test.c',
              'player/player_test_stubs.c',
              'hw_mmio/mmio_mock.c',
              '../player/player.c',
              '../kernel/pcm_ring.c'],
    include_directories: include_directories(
      '../player', '../hal', '../hal/hw', '../fs', '../kernel',
      '../codecs', '../codecs/dr_flac', '../codecs/dr_mp3',
      'hw_mmio', 'player', '.'),
    c_args: ['-DMMIO_MOCK', '-DCORE_FREESTANDING'],
    override_options: test_opts,
  )
  test('player-clock', player_clock_test, suite: 'unit')

  if py.found()
    # fat32-corrupt: the FAT32 reader against deliberately broken volumes.
    # fat32_test is entirely happy-path, yet this parser consumes whatever is
    # on a user's disk. Each image is one realistic way that goes wrong — a
    # looping FAT chain, cluster numbers outside the data region, a genuine
    # FAT16 boot sector, LFN runs with a mismatched checksum or no 8.3 entry,
    # and a volume truncated mid-transfer — and the assertion is always the
    # same: fail cleanly and bounded, never hang, never read out of bounds.
    # Boundedness is measured by a counting block device rather than a
    # timeout, so a non-terminating walk is a deterministic finding.
    fat32_corrupt_imgs = []
    foreach v : ['good', 'cyclic-fat', 'oob-cluster', 'fat16-bpb',
                 'orphan-lfn', 'truncated', 'long-lfn']
      fat32_corrupt_imgs += custom_target('fat32-img-' + v,
        output: v + '.img',
        command: [py, files('scripts' / 'make_fat32_image.py'),
                  '@OUTPUT@', '--variant', v])
    endforeach

    fat32_corrupt_test = executable('fat32_corrupt_test',
      sources: ['fs/fat32_corrupt_test.c', '../fs/fat32.c'],
      include_directories: include_directories('../fs', '.'),
      override_options: test_opts)
    test('fat32-corrupt', fat32_corrupt_test,
         args: [meson.current_build_dir()],
         depends: fat32_corrupt_imgs,
         suite: 'unit')

    # hw-name-hash-parity: the Python half of the locator-hash test —
    # tools/build_index.py against the same golden vectors the C test
    # (name-hash, above, on the real library/names.c) asserts.
    test('hw-name-hash-parity', py,
         args: [meson.current_source_dir() / 'scripts' /
                'check_name_hash_parity.py'],
         suite: 'unit')

    # hw-resume-parity: the same guard for the resume matcher — the copy in
    # tests/kernel/resume_test.c must not drift from kernel/main.c, and
    # RESUME_DUR_SLOP must agree. A drifted copy is worse than no test: it
    # passes while the device applies a different rule.
    test('hw-resume-parity', py,
         args: [meson.current_source_dir() / 'scripts' /
                'check_resume_parity.py'],
         suite: 'unit')

    # hw-index-loader: library_load_index() in kernel/main.c must actually
    # call the validator idx_test covers (idx_header_parse with the directory
    # entry's size, crc32_update, a refusal on IDX_ECRC) — a validator the
    # loader does not consult tests nothing.
    test('hw-index-loader', py,
         args: [meson.current_source_dir() / 'scripts' /
                'check_index_loader.py'],
         suite: 'unit')

    # build-index: the host tool that writes CORELIB.IDX, run over a synthetic
    # tree with ffprobe stubbed. Pins the v2 header and CRC the device
    # verifies, the filename enumeration the files on the device were named
    # by, and the track-number rules (tags first; a year or an artist's
    # leading digits are not a track number; disc tags are read).
    test('build-index', py,
         args: [meson.current_source_dir() / 'scripts' /
                'check_build_index.py'],
         suite: 'unit')
  endif
endif
