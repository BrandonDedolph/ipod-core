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
  # frame, bit-compare to the reference PCM (pvmp3's own captured output, the
  # KAT truth for a lossy codec), and assert the arena stayed inside the budget
  # player.c sizes for and never OOM'd. Same mp3.c + arena.c the ARM build
  # compiles; only the byte source differs (memory vs disk).
  mp3_fs_test = executable(
    'mp3_freestanding_test',
    sources: ['codecs/mp3_freestanding_test.c', '../codecs/arena.c'],
    dependencies: [mp3_dep],
    include_directories: include_directories('../codecs', '../codecs/pvmp3'),
    override_options: test_opts,
  )
  test('mp3-freestanding', mp3_fs_test, args: [vectors_dir], suite: 'unit')

  # mp3-frame: the framing layer (codecs/pvmp3/mp3_frame.c) on its own — the
  # 4-byte header across every version/layer/bitrate/rate/padding combination,
  # the ID3v2 prefix in all three versions with footers and chained tags, the
  # ID3v1/APE tail, Xing vs Info vs VBRI with every flag combination, the LAME
  # delay/padding fields, the TOC interpolation and the CBR estimate, plus a
  # streaming scan over a synthetic file with a junk prefix (two-header
  # validation). Pure functions over bytes: no vectors, no decoder.
  mp3_frame_test = executable(
    'mp3_frame_test',
    sources: ['codecs/mp3_frame_test.c', '../codecs/pvmp3/mp3_frame.c'],
    include_directories: include_directories('../codecs', '../codecs/pvmp3'),
    override_options: test_opts,
  )
  test('mp3-frame', mp3_frame_test, suite: 'unit')

  # id3-meta: the ID3 reader (codecs/pvmp3/id3_meta.c) — the SAME source the
  # ARM build links into core.elf. Builds tags in RAM and asserts the text
  # frames land in flac_meta_t: v2.2/v2.3/v2.4 layouts, all four encodings
  # (including real UTF-16 to UTF-8 in both endiannesses, with and without a
  # BOM, and a surrogate pair), TCON's numeric forms against the ID3v1 genre
  # table, "7/12" track numbers, an extended header, an APIC skipped by size,
  # an unsynchronised tag, and sizes that run off the end.
  id3_meta_test = executable(
    'id3_meta_test',
    sources: ['codecs/id3_meta_test.c',
              '../codecs/pvmp3/id3_meta.c',
              '../codecs/pvmp3/mp3_frame.c',
              '../codecs/meta_text.c'],
    include_directories: include_directories('../codecs', '../codecs/pvmp3'),
    override_options: test_opts,
  )
  test('id3-meta', id3_meta_test, suite: 'unit')

  # mp3-accuracy: is the decode RIGHT? The KAT only pins it against itself, so
  # this one compares pvmp3 against ffmpeg's decoder on real music — CBR, VBR
  # with a Xing TOC, and an MPEG-2 mono file — to the ISO 11172-4 full-accuracy
  # bounds (PSNR >= 96 dB, peak error <= 2 LSB after alignment). The reference
  # PCM is stored as FLAC and decoded here with the tree's own dr_flac, which
  # is lossless and half the size. See codec-vectors/README.md for provenance.
  mp3_accuracy_test = executable(
    'mp3_accuracy_test',
    sources: ['codecs/mp3_accuracy_test.c'],
    dependencies: [mp3_dep, flac_dep, m_dep],
    include_directories: include_directories('../codecs', '../codecs/pvmp3',
                                             '../codecs/dr_flac'),
    override_options: test_opts,
  )
  test('mp3-accuracy', mp3_accuracy_test, args: [vectors_dir], suite: 'unit')

  # mp3-seek: seeking by the Xing TOC. Seeks the VBR vector to a set of
  # targets, decodes a slice at each, and finds where that slice actually sits
  # in the reference by correlation — so the assertion is about the audio the
  # listener would hear, not about arithmetic agreeing with itself. Landing
  # error is held to one frame plus the TOC's 1 %-of-file granularity, and a
  # seek past the end must leave the decoder at a clean EOS.
  mp3_seek_test = executable(
    'mp3_seek_test',
    sources: ['codecs/mp3_seek_test.c'],
    dependencies: [mp3_dep, flac_dep, m_dep],
    include_directories: include_directories('../codecs', '../codecs/pvmp3',
                                             '../codecs/dr_flac'),
    override_options: test_opts,
  )
  test('mp3-seek', mp3_seek_test, args: [vectors_dir], suite: 'unit')

  # mp3-robust: the inputs a user's library actually contains. A truncated
  # tail, a bit flipped inside a frame, junk spliced between frames, a Layer II
  # file, an 8 kHz MPEG-2.5 file (refused: the DAC cannot clock it), an ID3 tag
  # claiming more bytes than the file holds, and a file that is not MPEG at
  # all. Every case must terminate with a defined answer and never read out of
  # bounds — run under the ASan/UBSan job like every other suite.
  mp3_robust_test = executable(
    'mp3_robust_test',
    sources: ['codecs/mp3_robust_test.c'],
    dependencies: [mp3_dep],
    include_directories: include_directories('../codecs', '../codecs/pvmp3'),
    override_options: test_opts,
  )
  test('mp3-robust', mp3_robust_test, args: [vectors_dir], suite: 'unit')

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
    sources: ['codecs/flac_meta_test.c', '../codecs/flac_meta.c',
              '../codecs/meta_text.c'],
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
  # letter-mode latch surviving a slowdown AND a pause the plate outlives
  # (while the same pause with nothing latched is still a plain reset), the
  # letterless-screen fall-through, letter stepping at both ends, the letter-
  # step seam standing in for the run index, the plate's hold window to the
  # microsecond, and a gap across the 32-bit clock wrap.
  wheel_test = executable(
    'wheel_test',
    sources: ['ui/wheel_test.c', '../ui/wheel.c'],
    include_directories: include_directories('../ui', '.'),
    override_options: test_opts,
  )
  test('wheel', wheel_test, suite: 'unit')

  # letterindex: the A-Z run index (core/ui/letterindex.c) — the SAME source
  # the ARM build links into core.elf. It is the piece that decides whether a
  # list gets an A-Z plate at all and where a letter detent lands, and both
  # judgements are invisible on the device: an index that wrongly said "this
  # list is not sorted" would look exactly like a feature nobody had built.
  # The per-row initial is injected, so each "list" here is a string of
  # letters — which is how the collation cases that actually bite (a '#' group
  # BEFORE A and a second one AFTER Z, rows in no order at all, an unsorted
  # list) each cost one literal. Sentinels bracket the index struct, and the
  # step cases are wheel_test §6's, because Stage B swaps the walk for this.
  letterindex_test = executable(
    'letterindex_test',
    sources: ['ui/letterindex_test.c', '../ui/letterindex.c'],
    include_directories: include_directories('../ui', '.'),
    override_options: test_opts,
  )
  test('letterindex', letterindex_test, suite: 'unit')

  # search: Music > Search (core/ui/search.c) — the SAME source the ARM build
  # links into core.elf. It is the first screen here whose whole value is a
  # JUDGEMENT rather than a fact — which of two matches the person meant — and
  # none of that is visible from a screenshot: "Sunflower Bean" above "The
  # Sun", the one artist above two hundred songs, an accented album reachable
  # from an unaccented query, an apostrophe the 39-cell ring cannot type
  # crossed anyway, and the cap admitting how much it hid. The library is
  # injected, so the fixture is thirty names chosen for exactly those cases.
  # §6-8 are the painter in chrome_test.c's style — render into console_fb()
  # and read the pixels back — because the layout's claims (the status band is
  # untouched, the scrollbar column is untouched in PICK, the footer appears
  # only when something was hidden) are the ones a renderer breaks by accident.
  search_test = executable(
    'search_test',
    sources: ['ui/search_test.c', '../ui/search.c', '../ui/chrome.c',
              '../ui/text.c', '../ui/palette.c', '../library/fold.c',
              '../library/names.c', '../kernel/console.c'],
    include_directories: include_directories('../ui', '../kernel', '../hal', '.'),
    override_options: test_opts,
  )
  test('search', search_test, suite: 'unit')

  # keyhold: tap-vs-hold press-length arbitration (core/ui/keyhold.c) — the
  # SAME source the ARM build links into core.elf, driving PLAY (tap = pause,
  # hold = sleep), MENU (tap = back, hold = the main menu) and, through
  # ui/gesture.c, RIGHT/LEFT (tap = skip, hold = seek). Pure integer logic
  # over (is_down, clock) samples. Asserts that a press yields exactly ONE
  # action decided by its length: silent at the down-edge, TAP on release
  # under the threshold, HOLD on the first pass at it, a silent release after
  # a HOLD (the wake case), the swallowed-tap rule (a press that woke the
  # backlight taps nothing but can still hold), the VOID rule (a press that
  # dismissed a modal produces neither action), keyhold_held() through a
  # press, two instances with different thresholds not disturbing each other,
  # the mid-press reset, and a press across the 32-bit clock wrap.
  keyhold_test = executable(
    'keyhold_test',
    sources: ['ui/keyhold_test.c', '../ui/keyhold.c'],
    include_directories: include_directories('../ui', '.'),
    override_options: test_opts,
  )
  test('keyhold', keyhold_test, suite: 'unit')

  # gesture: the press-length GESTURES (core/ui/gesture.c) — the SAME source
  # the ARM build links into core.elf. The RIGHT/LEFT skip-vs-seek machine
  # (its latched "allowed" rule, the aim, the ramp table to the microsecond,
  # the end-of-track clamps, the single commit on release, cancel and void)
  # and the PLAY-tap policy (which screens start a queue and which pause).
  # Every case is a sequence of (is_down, clock, allowed, elapsed, total)
  # samples fed the way the main loop feeds it, and the assertions are the
  # action and the exact aim target in seconds.
  gesture_test = executable(
    'gesture_test',
    sources: ['ui/gesture_test.c', '../ui/gesture.c', '../ui/keyhold.c'],
    include_directories: include_directories('../ui', '.'),
    override_options: test_opts,
  )
  test('gesture', gesture_test, suite: 'unit')

  # sleeptimer: the sleep timer's countdown (core/ui/sleeptimer.c) — the SAME
  # source the ARM build links into core.elf. Pure integer arithmetic over
  # (clock) samples, and the reason it is a unit at all is the 1 MHz
  # USEC_TIMER's ~71.6-minute 32-bit wrap: a 120-minute countdown crosses it
  # TWICE, which nothing on the device can be asked to demonstrate. Asserts
  # the minute boundaries, that FIRE happens exactly once and disarms, the
  # full two-hour run across both wraps at 100 ms per feed, that coarse and
  # irregular feeds carry their remainder instead of drifting, re-arming, the
  # token's text and its bounds check, and that remaining is never 0 while
  # armed.
  sleeptimer_test = executable(
    'sleeptimer_test',
    sources: ['ui/sleeptimer_test.c', '../ui/sleeptimer.c'],
    include_directories: include_directories('../ui', '.'),
    override_options: test_opts,
  )
  test('sleeptimer', sleeptimer_test, suite: 'unit')
  # jackwatch: the pause-on-unplug policy (core/ui/jackwatch.c) — the SAME
  # source the ARM build links into core.elf, fed one debounced jack level per
  # main-loop pass. It replaced five untested lines inline in kernel/main.c,
  # which is not host-built, so these are the first assertions this decision
  # has ever had: -1 ("the detect line is not trusted on this device") is not
  # a level and can neither prime nor pause; a boot with an empty jack is a
  # prime, not a pull-out; a pull while playing is exactly one PAUSE; a pull
  # while paused is only an OUT; re-inserting is only an IN, NEVER a resume; the wake re-prime
  # moves the believed level with no action; raw-edge bookkeeping counts
  # transitions but not the first reading; and the module does no timing of
  # its own (a pull across the 32-bit clock wrap still pauses).
  jackwatch_test = executable(
    'jackwatch_test',
    sources: ['ui/jackwatch_test.c', '../ui/jackwatch.c'],
    include_directories: include_directories('../ui', '.'),
    override_options: test_opts,
  )
  test('jackwatch', jackwatch_test, suite: 'unit')

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
  # Root rows return the right ENTER_*/RESET action codes plus value/kind report,
  # and that Sleep Timer cycles Off/15/30/60/90/120 reporting the SLEEPTIMER
  # action — "changed, but runtime-only", the code that keeps arming it off the
  # user's disk. kernel/datetime.c is linked because Date & Time's first row
  # formats the injected clock through it.
  settings_test = executable(
    'settings_test',
    sources: ['ui/settings_test.c', '../ui/settings.c', '../ui/eq.c',
              '../kernel/datetime.c'],
    include_directories: include_directories('../ui', '.'),
    override_options: test_opts,
  )
  test('settings', settings_test, suite: 'unit')

  # eq: the equaliser preset table (core/ui/eq.c) — the SAME source the ARM
  # build links into core.elf. Pure: names, five band gains and five centre
  # codes per preset. Asserts the gains against an independently typed fixture,
  # the structural limits hal_eq_set's register encoder relies on (+/-12 dB,
  # 2-bit centre codes, no bandwidth bit on either shelf), the pre-cut rule,
  # and who owns the shelves when a preset is on.
  eq_test = executable(
    'eq_test',
    sources: ['ui/eq_test.c', '../ui/eq.c'],
    include_directories: include_directories('../ui'),
    override_options: test_opts,
  )
  test('eq', eq_test, suite: 'unit')

  # settime: the Set Date & Time field editor's model (core/ui/settime.c) — the
  # SAME source the ARM build links into core.elf. A field editor is nothing
  # but edge cases and the only way to find them at the bench is to stand there
  # turning a wheel, so: 31 January stepped into February (and into a leap
  # February), the year clamping where every other field wraps, midnight and
  # noon both reading 12, an AM/PM plate that moves the hour by exactly twelve
  # without dragging the hour plate with it, and Select reporting "done"
  # exactly once, on the last plate, in both 12- and 24-hour mode.
  settime_test = executable(
    'settime_test',
    sources: ['ui/settime_test.c', '../ui/settime.c', '../kernel/datetime.c'],
    include_directories: include_directories('../ui', '.'),
    override_options: test_opts,
  )
  test('settime', settime_test, suite: 'unit')

  # palette: the seven theme tables + theme_set() (core/ui/palette.c) — the SAME
  # source the ARM build links into core.elf. Asserts every table is fully
  # populated, that light themes are light and dark themes dark (the selection
  # bar is derived from ink/surface, so polarity is what keeps it legible), the
  # derived-tone contract documented in palette.h, and that theme_set() loads
  # exactly the named table with anything out of range landing on Linen.
  palette_test = executable(
    'palette_test',
    sources: ['ui/palette_test.c', '../ui/palette.c'],
    include_directories: include_directories('../ui'),
    override_options: test_opts,
  )
  test('palette', palette_test, suite: 'unit')

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
  # single-runnable no-switch case. The context-switch asm itself is not
  # reachable from here — it is covered by the boot-smoke CI job, which boots
  # the linked image in clicky and requires `core: idle task entered`, a line
  # printed from inside a task reached only through switch_context and
  # task_trampoline (tests/clicky/boot_uart.golden).
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
  # tick so the cooperative wait terminates). The IRQ entry asm and real
  # interrupt delivery are not reachable host-side; they are covered by the
  # boot-smoke CI job, whose settle window requires the run to survive live
  # timer IRQs through our own vector table — a broken entry lands in the
  # fault vectors, which panic over the same UART and fail the check.
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
  # clock.c now also orchestrates the suspend-time DEV_EN gating through
  # the owning drivers (uart/piezo/i2c), so they link in; none of them
  # emits bus traffic unless a gate function is called.
  clock_test = executable(
    'clock_test',
    sources: ['hw_clock/clock_test.c',
              '../kernel/clock.c',
              '../hal/hw/uart.c',
              '../hal/hw/piezo.c',
              '../hal/hw/i2c.c',
              'hw_mmio/mmio_mock.c'],
    include_directories: include_directories('../hal', '../hal/hw', '../kernel', 'hw_mmio'),
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-clock', clock_test, suite: 'unit')

  # hw-clock-gate: the suspend-time DEV_EN hygiene — clock_gate_suspend
  # clears exactly SER0/PWM/I2C (one masked RMW each, DEV_OPTO and the
  # ROM's bits untouched), resume restores only what was on, and each
  # driver re-gates itself on its next use so nothing can spin against an
  # unclocked block. The i2c/uart golden traces elsewhere never suspend
  # and are unchanged by this.
  gate_test = executable(
    'gate_test',
    sources: ['hw_clock/gate_test.c',
              '../kernel/clock.c',
              '../hal/hw/uart.c',
              '../hal/hw/piezo.c',
              '../hal/hw/i2c.c',
              'hw_mmio/mmio_mock.c'],
    include_directories: include_directories('../hal', '../hal/hw', '../kernel', 'hw_mmio'),
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-clock-gate', gate_test, suite: 'unit')

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

  # evlog: the on-disk event log (kernel/evlog.c) — the SECOND writer to the
  # user's disk, so it gets the config test's treatment over the same kind of
  # in-RAM volume (BytesPerSector 2048 at a non-zero partition base), with
  # CORELOG.BIN deliberately FRAGMENTED so the chain walk is load-bearing:
  #   1. the block format: header + ring-block round trips, every rejection,
  #      the slot arithmetic — and the host tool's --emit fixture decoded by
  #      the firmware's decoder (host/device format agreement, as config);
  #   2. addressing: every block's LBA against the test's own chain formula;
  #      the recorder refuses any write outside the file's clusters, any
  #      misaligned write, and any write to block 0;
  #   3. mount: fresh / absent / bad header / size disagreement / ragged /
  #      unreadable (retried) / short chain refuses blocks past it;
  #   4. the flush policy through cfg_commit's gate: idle waits for a full
  #      block and the debounce and never wakes a parked drive, forced
  #      writes a partial FINAL block and wakes first, the battery gate,
  #      one block per call, failed writes, three failures = OFF, ring
  #      overflow with its marker;
  #   5. ring order and the boot scan: wrap, remount, boot id, torn blocks
  #      at and off the cursor, an unreadable slot = OFF, the python-written
  #      ring scanned, and the 64-slot ring proving the search is O(log n).
  # Links the real cfg_commit.c and fat32.c; the write and wake are the
  # test's recorder, so no ata.c — the register grammar of a 4-sector write
  # is ata_trace_test's job.
  evlog_test = executable(
    'evlog_test',
    sources: ['kernel/evlog_test.c',
              '../kernel/evlog.c',
              '../kernel/cfg_commit.c',
              '../fs/fat32.c'],
    include_directories: [hw_mmio_inc,
                          include_directories('../kernel', '../fs')],
    override_options: test_opts,
  )
  if py.found()
    evlog_fixture = custom_target('evlog-fixture',
      output: 'evlog_golden.bin',
      command: [py, files('..' / '..' / 'tools' / 'make_log.py'),
                '--emit', '@OUTPUT@'])
    test('evlog', evlog_test, args: [evlog_fixture], suite: 'unit')
  else
    test('evlog', evlog_test, suite: 'unit')
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
  # and pixel packing against the recording mock bus. Also links the
  # console (damage rect) so the Now Playing partials — transport band and
  # volume plate, geometry from ui/chrome.h — are pinned to their word
  # counts through the same damage -> lcd_present_rect chain main.c uses.
  lcd_present_test = executable(
    'lcd_present_test',
    sources: ['hw_mmio/lcd_present_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/lcd.c',
              '../kernel/console.c'],
    include_directories: include_directories('../hal', '../hal/hw', 'hw_mmio',
                                             '../kernel', '../ui'),
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

  # hw-rtc: the PCF50605 real-time clock (hal/hw/rtc.c over the same I2C
  # register-pointer path the gauge uses), host-side under -DMMIO_MOCK. The RTC
  # register map is NOT in docs/hw/ — it is datasheet-derived and unconfirmed
  # on hardware — so this suite pins everything around that uncertainty: the
  # exact read and write grammar (SC..WD, DT..YR, SC again; seven single-register
  # writes seconds-first), the torn-read retry at a second boundary, and that
  # every shape of wrong answer (bad BCD, the year-00 reset value, the NACK
  # shape where i2c_read hands back the DATA registers' own last bytes) decodes
  # as "unset" rather than as a plausible wrong date. kernel/datetime.c is
  # linked because the driver converts through it.
  rtc_trace_test = executable(
    'rtc_trace_test',
    sources: ['hw_mmio/rtc_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/rtc.c',
              '../hal/hw/i2c.c',
              '../kernel/datetime.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK'],
    override_options: test_opts,
  )
  test('hw-rtc', rtc_trace_test, suite: 'unit')

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
  #   untrusted  (TRUSTED=0, what ships until the pin has been read on the
  #              device): answers -1 and never touches the bus.
  #
  # Both non-probe binaries also pin headphone_pin_cfg(), the two-read,
  # no-write accessor the About screen's on-screen probe draws from.
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

  # TRUSTED=0 is spelled out rather than inherited from the header default.
  # The bench flips that default to 1 the moment the polarity is read off the
  # About screen, and an inherited default would quietly turn this binary into
  # a SECOND trusted build — main() would compile the trusted branch, the
  # suite would stay green, and the "-1 and not one bus cycle" promise that is
  # the whole reason this binary exists would stop being tested by anything.
  headphone_untrusted_test = executable(
    'headphone_untrusted_test',
    sources: ['hw_mmio/headphone_trace_test.c',
              'hw_mmio/mmio_mock.c',
              '../hal/hw/headphone.c'],
    include_directories: hw_mmio_inc,
    c_args: ['-DMMIO_MOCK', '-DHEADPHONE_DETECT_TRUSTED=0'],
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

  # fold: the search fold (core/library/fold.c) — the SAME source the ARM
  # build links into core.elf. It exists because name_hash() must NOT grow a
  # diacritic rule (its output is the on-disk locator, computed independently
  # by tools/build_index.py and pinned on both sides), and the price of a
  # second folder is that it has to AGREE with the first on the rules they
  # share — which §4 asserts through name_hash itself rather than by reading
  # both functions. The rest is the 192-entry table, spot-checked at every
  # seam including the two non-letters inside it, plus fold_ascii's bounds
  # under sentinels and the 1:1 codepoint-to-byte property ui/search.c relies
  # on, checked over the whole BMP.
  fold_test = executable(
    'fold_test',
    sources: ['library/fold_test.c', '../library/fold.c', '../library/names.c'],
    include_directories: include_directories('../library', '.'),
    override_options: test_opts,
  )
  test('fold', fold_test, suite: 'unit')

  # playlist: the Playlists read path (core/library/playlist.c) — the SAME
  # source the ARM build links. Lists <library root>/Playlists/*.m3u8 and
  # resolves one into playable rows over the real fs/m3u.c + fs/fat32.c, on
  # a hand-built in-RAM volume with VFAT long names: every entry shape a
  # desktop tool writes, every way an entry fails (missing, a folder, not
  # audio, a read error) as a COUNT, the row and playlist caps reported, and
  # an unreadable playlist file as the one hard error.
  playlist_test = executable(
    'playlist_test',
    sources: ['library/playlist_test.c', '../library/playlist.c',
              '../library/names.c', '../fs/m3u.c', '../fs/fat32.c'],
    include_directories: include_directories('.'),
    override_options: test_opts,
  )
  test('playlist', playlist_test, suite: 'unit')

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
  # calls is the real library/names.c, and the queue-context seam (which
  # list a view is, the seeded Shuffle Songs order, what a capture stores)
  # is the real header-only kernel/resume_ctx.h.
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

  # cfg-commit: the settings-save commit gate (core/kernel/cfg_commit.c) —
  # the SAME source the ARM build links. It decides when a pending settings
  # change becomes the firmware's one disk write: the debounce, the "don't
  # spin the platters up for 1 KB under a live player" refusal, the battery
  # gate and its DISKSAFE exemption, and — the two it got wrong while it was
  # a static in main.c — that a write into a PARKED drive is preceded by a
  # wake, and that a write that FAILS leaves the change pending for a retry
  # (one debounce later, bounded) instead of discarding it.
  cfg_commit_test = executable(
    'cfg_commit_test',
    sources: ['kernel/cfg_commit_test.c', '../kernel/cfg_commit.c'],
    include_directories: include_directories('.'),
    override_options: test_opts,
  )
  test('cfg-commit', cfg_commit_test, suite: 'unit')

  # datetime: the civil<->epoch calendar (core/kernel/datetime.c) — the SAME
  # source the ARM build links into core.elf. The clock's only purely
  # arithmetic part, and the only one that can be settled COMPLETELY off the
  # device, so it is: a hand table of dates derived outside this codebase (the
  # epoch origin, both sides of 29 February in 2000 and 2004, the signed
  # 32-bit roll, the last second of 2099) and then an EXHAUSTIVE walk of all
  # 36 525 days from 2000-01-01 to 2099-12-31 in both directions, at both ends
  # of each day, against an independently accumulated epoch and weekday. Plus
  # the 12/24-hour formatter's edge hours and the local-offset conversion.
  datetime_test = executable(
    'datetime_test',
    sources: ['kernel/datetime_test.c', '../kernel/datetime.c'],
    include_directories: include_directories('../kernel', '.'),
    override_options: test_opts,
  )
  test('datetime', datetime_test, suite: 'unit')

  # timesync: what the firmware does at boot with the clock stamp the host
  # leaves in CORECFG.DAT (core/kernel/timesync.c), and the software clock that
  # carries the time between the rare RTC reads (core/kernel/wallclock.c) —
  # both the SAME sources the ARM build links. Neither is testable on the
  # device: the decision's two failure modes (applying a stale stamp on every
  # boot, a lost mark making a stamp immortal) only appear across boots days
  # apart, and the wallclock's whole difficulty is the 1 MHz counter's ~71.6
  # minute wrap, which a two-hour run here crosses twice in milliseconds.
  timesync_test = executable(
    'timesync_test',
    sources: ['kernel/timesync_test.c', '../kernel/timesync.c',
              '../kernel/wallclock.c'],
    include_directories: include_directories('../kernel', '.'),
    override_options: test_opts,
  )
  test('timesync', timesync_test, suite: 'unit')

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
      '../codecs', '../codecs/dr_flac', '../codecs/pvmp3',
      'hw_mmio', 'player', '.'),
    c_args: ['-DMMIO_MOCK', '-DCORE_FREESTANDING'],
    override_options: test_opts,
  )
  test('player-queue', player_queue_test, suite: 'unit')

  # player-shuffle: the shuffle order as a VALUE — a pure function of the
  # (seed, keep) pair the player records for every deal, so the resume path
  # can save 6 bytes and deal the same "what plays next" back after a power
  # cut. Same fakes as player-queue; the mock USEC timer is the player's only
  # entropy, so every sequence asserted here is fixed, not a coin toss.
  player_shuffle_test = executable(
    'player_shuffle_test',
    sources: ['player/player_shuffle_test.c',
              'player/player_test_stubs.c',
              'hw_mmio/mmio_mock.c',
              '../player/player.c',
              '../kernel/pcm_ring.c'],
    include_directories: include_directories(
      '../player', '../hal', '../hal/hw', '../fs', '../kernel',
      '../codecs', '../codecs/dr_flac', '../codecs/pvmp3',
      'hw_mmio', 'player', '.'),
    c_args: ['-DMMIO_MOCK', '-DCORE_FREESTANDING'],
    override_options: test_opts,
  )
  test('player-shuffle', player_shuffle_test, suite: 'unit')

  # player-album: Shuffle Albums — the deal whose unit is an ALBUM. The
  # fixture interleaves three albums in the queue AND scrambles their track
  # numbers, so a pass that only regroups (or only reorders) fails it. Same
  # fakes as player-queue; what is asserted is the sequence of files that
  # would actually play, never the order array.
  player_album_test = executable(
    'player_album_test',
    sources: ['player/player_album_test.c',
              'player/player_test_stubs.c',
              'hw_mmio/mmio_mock.c',
              '../player/player.c',
              '../kernel/pcm_ring.c'],
    include_directories: include_directories(
      '../player', '../hal', '../hal/hw', '../fs', '../kernel',
      '../codecs', '../codecs/dr_flac', '../codecs/pvmp3',
      'hw_mmio', 'player', '.'),
    c_args: ['-DMMIO_MOCK', '-DCORE_FREESTANDING'],
    override_options: test_opts,
  )
  test('player-album', player_album_test, suite: 'unit')

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
      '../codecs', '../codecs/dr_flac', '../codecs/pvmp3',
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

    # make-log: the host half of the on-disk event log (tools/make_log.py).
    # Round-trips a synthesized ring through --dump: blocks come back in
    # sequence order regardless of the slot the ring put them in, an
    # overwritten or torn block is reported as a gap, boot boundaries and
    # the FINAL flag show, a bad header is refused. The device side of the
    # same format is tests/kernel/evlog_test.c, which decodes this tool's
    # --emit fixture with the firmware's decoder.
    test('make-log', py,
         args: [meson.current_source_dir() / '..' / '..' / 'tools' /
                'make_log.py', '--selftest'],
         suite: 'unit')
  endif
endif
