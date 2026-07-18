# Test harness: codec known-answer tests.
#
# Builds codec_kat as a host executable; the fixture directory is
# passed at runtime. `meson test` picks them up.

if target == 'sim'
  # libm needed only on platforms where it's a separate library;
  # macOS and most BSDs link math via libSystem and reject -lm.
  cc = meson.get_compiler('c')
  m_dep = cc.find_library('m', required: false)

  codec_kat = executable(
    'codec_kat',
    sources: ['codec_kat.c'],
    dependencies: [codecs_dep, m_dep],
  )

  vectors_dir = meson.current_source_dir() / 'codec-vectors'
  # codec_kat runs every vendored codec's KAT in one binary; one
  # meson test entry covers all of them. Failures print which codec
  # failed via the [codec] label in stderr.
  test('codec-kat', codec_kat,
       args: [vectors_dir],
       suite: 'unit')

  # tcdb-reader: hand-rolls a minimal .tcdb byte string in memory
  # (using the same constants the parser reads) and verifies the
  # parser populates the public tagcache_* accessors correctly.
  # Independent from the Go encoder — both sides implement the
  # spec separately, so a layout drift fails this AND the Go
  # round-trip test simultaneously.
  tcdb_reader_test = executable(
    'tcdb_reader_test',
    sources: ['tcdb_reader_test.c'],
    dependencies: [tagcache_dep],
  )
  test('tcdb-reader', tcdb_reader_test, suite: 'unit')

  tag_mp3_test = executable(
    'tag_mp3_test',
    sources: ['tag_mp3_test.c'],
    dependencies: [codecs_dep],
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
    c_args: ['-DMMIO_MOCK', '-Wno-unterminated-string-initialization'],
  )
  test('hw-uart-trace', uart_trace_test, suite: 'unit')

  lcd_trace_test = executable(
    'lcd_trace_test',
    sources: ['hw_mmio/lcd_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/lcd.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK', '-Wno-unterminated-string-initialization'],
  )
  test('hw-lcd-trace', lcd_trace_test, suite: 'unit')

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
  # single-runnable no-switch case. The context-switch asm itself is
  # exercised end-to-end in the clicky emulator, not here.
  sched_test = executable(
    'sched_test',
    sources: ['hw_sched/sched_test.c',
              'hw_sched/sched_test_mocks.c',
              '../kernel/sched.c'],
    include_directories: include_directories('../kernel'),
    c_args: ['-DSCHED_TEST'],
  )
  test('hw-sched', sched_test, suite: 'unit')

  # hw-timer: the interrupt dispatcher + 100 Hz timer tick, compiled
  # host-side with -DMMIO_MOCK against the recording fake bus. Asserts
  # timer_init's exact register grammar (arm CFG + unmask), that
  # timer_tick_isr acks by reading TIMER1_VAL and advances the counter,
  # that irq_dispatch only ticks when CPU_INT_STAT shows TIMER1 pending,
  # and the sleep_ms tick math (a sched_yield stub advances the mocked
  # tick so the cooperative wait terminates). The IRQ entry asm + real
  # interrupt delivery are exercised end-to-end in clicky.
  timer_test = executable(
    'timer_test',
    sources: ['hw_timer/timer_test.c',
              'hw_timer/timer_test_stubs.c',
              '../kernel/timer.c',
              '../kernel/irq.c',
              'hw_mmio/mmio_mock.c'],
    include_directories: include_directories('../hal', '../hal/hw', '../kernel', 'hw_mmio'),
    c_args: ['-DMMIO_MOCK'],
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
              '../hal/hw/i2s.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
  )
  test('hw-audio', audio_trace_test, suite: 'unit')

  # hw-console: the on-screen debug console (framebuffer + 8x8 hex font).
  # Pure rendering logic, no MMIO — verifies glyph output pixel-for-pixel.
  console_test = executable(
    'console_test',
    sources: ['hw_console/console_test.c', '../kernel/console.c'],
    include_directories: include_directories('../kernel', '../hal'),
  )
  test('hw-console', console_test, suite: 'unit')

  # hw-lcd-present: lcd_present() streams a caller framebuffer over the
  # BCM (same path as lcd_fill) — asserts the exact 38400-word grammar
  # and pixel packing against the recording mock bus.
  lcd_present_test = executable(
    'lcd_present_test',
    sources: ['hw_mmio/lcd_present_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/lcd.c'],
    include_directories: include_directories('../hal', '../hal/hw', 'hw_mmio'),
    c_args: ['-DMMIO_MOCK', '-Wno-unterminated-string-initialization'],
  )
  test('hw-lcd-present', lcd_present_test, suite: 'unit')

  # End-to-end audio-playback test: spawns core-sim with the SDL2
  # disk audio driver, plays the FLAC fixture, then bit-compares
  # the captured PCM to the codec_kat reference. Exercises the
  # tagcache → cabinet → audio_engine_play → ring → HAL audio
  # callback path that codec_kat alone can't reach.
  #
  # The codec-vectors/ source dir doubles as the --music scan dir:
  # it contains exactly one .flac and one .mp3, so title-sort puts
  # the .flac at row 0 (the press sequence selects row 0). Capture
  # is written under the meson temp dir so multiple test runs in
  # parallel can't clobber each other.
  sim_audio_playback = executable(
    'sim_audio_playback',
    sources: ['sim_audio_playback.c'],
  )

  test('sim-audio-playback', sim_audio_playback,
       args: [
         core_sim_target.full_path(),
         vectors_dir,        # fixture-dir (sim --music)
         vectors_dir,        # vectors-dir (where the reference PCM lives)
       ],
       depends: [core_sim_target],
       suite:   'integration',
       timeout: 30)
endif
