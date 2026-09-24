/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_audio/audio_trace_test.c — host-side golden-trace tests for
 * the first-sound driver chain (i2c.c, wm8758.c, i2s.c), compiled
 * against the recording mock bus (-DMMIO_MOCK). This is the ONLY
 * automated check of the audio register grammar: clicky models no
 * I2C/I2S/DAC, and the device would need a logic analyzer. Sound itself
 * is the on-device proof; this proves the bytes are right.
 *
 * Proves:
 *   1. i2c_init emits the exact clock-gate/idle-wait/reset/poke grammar,
 *      waits out an in-flight transaction BEFORE the reset, and only
 *      drains the bus on every later call (the reset is one-shot).
 *   2. i2c_send emits the exact controller sequence (addr, write-mode,
 *      data, count, strobe) and rejects bad lengths / BUSY timeout.
 *   3. wm8758_init is the datasheet's power-up sequence: 30 codec writes
 *      with correct 9-bit framing, the DAC->mixer route present, VMID on
 *      the 75k divider with a 100 ms rise waited out before the output
 *      unmute, POBCTRL dropped LAST, the DAC left soft-muted; and its two
 *      companions — wm8758_powerdown drains VMID for 300 ms before the
 *      rails go, wm8758_retune re-clocks a warm codec without a reset.
 *   4. i2s_init emits the exact clock-plumbing + FIFO reset/format
 *      grammar.
 *   5. i2s_write_stereo polls TXFree then writes one packed [R<<16|L]
 *      word, and reports a never-draining FIFO instead of hanging.
 *
 * All expected values are hand-derived from core/docs/hw/05-audio.md and
 * 09-i2c.md (via pp5022.h / wm8758.h), never from Rockbox source.
 */

#include "pp5022.h"
#include "wm8758.h"
#include "i2c.h"
#include "i2s.h"
#include "dma.h"
#include "mmio_mock.h"
#include "trace_expect.h"

static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    return cond ? 0 : 1;
}

/* ---- log-scanning helpers ------------------------------------------- */

static size_t count_writes(uint32_t addr)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len(), c = 0;
    for (size_t i = 0; i < len; i++) {
        if (log[i].op == MMIO_OP_WRITE && log[i].addr == addr) {
            c++;
        }
    }
    return c;
}

/* Value of the n-th (0-based) write to `addr`; ~0u if there is none. */
static uint32_t nth_write(uint32_t addr, size_t n)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len(), c = 0;
    for (size_t i = 0; i < len; i++) {
        if (log[i].op == MMIO_OP_WRITE && log[i].addr == addr) {
            if (c == n) {
                return log[i].value;
            }
            c++;
        }
    }
    return ~0u;
}

/* Does some codec write carry payload byte0==b0 AND byte1==b1 at the same
 * transaction index? (DATA0 and DATA1 writes come in lockstep pairs.) */
static int has_codec_write(uint8_t b0, uint8_t b1)
{
    size_t pairs = count_writes(I2C_DATA0_ADDR);
    for (size_t i = 0; i < pairs; i++) {
        if (nth_write(I2C_DATA0_ADDR, i) == b0 &&
            nth_write(I2C_DATA1_ADDR, i) == b1) {
            return 1;
        }
    }
    return 0;
}

/* ---- cases ----------------------------------------------------------- */

/* From i2c.c, host-test-only (MMIO_MOCK-guarded): forget the one-shot init. */
extern void i2c_test_reset(void);

/* Case 1: i2c_init grammar. DEV_EN/DEV_RS RMW sources read 0, STATUS
 * reads idle, so the sequence is fully determined: gate on, the idle wait
 * that must precede the reset, reset pulse, the two clock-config pokes,
 * and the settling idle read. */
static int test_i2c_init_grammar(void)
{
    i2c_test_reset();
    mmio_mock_reset();
    mmio_mock_set_read(DEV_EN_ADDR,     0);
    mmio_mock_set_read(DEV_RS_ADDR,     0);
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);   /* idle */

    i2c_init();

    trace_cursor tc = trace_begin("i2c_init");
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, DEV_I2C);
    expect_r(&tc, 8,  I2C_STATUS_ADDR);            /* idle BEFORE reset */
    expect_r(&tc, 32, DEV_RS_ADDR);
    expect_w(&tc, 32, DEV_RS_ADDR, DEV_I2C);
    expect_r(&tc, 32, DEV_RS_ADDR);
    expect_w(&tc, 32, DEV_RS_ADDR, 0);
    expect_w(&tc, 32, I2C_CLKCFG_ADDR, 0x00000000);
    expect_w(&tc, 32, I2C_CLKCFG_ADDR, 0x00000080);
    expect_r(&tc, 8,  I2C_STATUS_ADDR);
    trace_expect_end(&tc);
    return trace_done(&tc);
}

/* Case 1b: the reset waits for an in-flight transaction. i2c_send returns
 * before its transaction completes, and hal_audio_init calls i2c_init right
 * behind wm8758_mute's write on every track change; a DEV_RS pulse landing
 * on that write truncates it mid-byte and can leave the codec holding SDA,
 * with no bit-bang path to free it. STATUS reads BUSY three times, then
 * idle: every one of those polls must come BEFORE the first DEV_RS write. */
static int test_i2c_init_waits_before_reset(void)
{
    static const uint32_t seq[] = { I2C_BUSY, I2C_BUSY, I2C_BUSY, 0 };
    i2c_test_reset();
    mmio_mock_reset();
    mmio_mock_set_read(DEV_EN_ADDR, 0);
    mmio_mock_set_read(DEV_RS_ADDR, 0);
    mmio_mock_queue_read(I2C_STATUS_ADDR, seq, 4);

    i2c_init();

    trace_cursor tc = trace_begin("i2c_init_waits_before_reset");
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, DEV_I2C);
    for (int i = 0; i < 4; i++) {
        expect_r(&tc, 8, I2C_STATUS_ADDR);         /* BUSY x3, then idle */
    }
    expect_r(&tc, 32, DEV_RS_ADDR);
    expect_w(&tc, 32, DEV_RS_ADDR, DEV_I2C);
    expect_r(&tc, 32, DEV_RS_ADDR);
    expect_w(&tc, 32, DEV_RS_ADDR, 0);
    expect_w(&tc, 32, I2C_CLKCFG_ADDR, 0x00000000);
    expect_w(&tc, 32, I2C_CLKCFG_ADDR, 0x00000080);
    expect_r(&tc, 8,  I2C_STATUS_ADDR);
    trace_expect_end(&tc);
    return trace_done(&tc);
}

/* Case 1c: re-init is a drain, not a reset. The second (and every later)
 * i2c_init only waits for the bus to go idle — no DEV_EN/DEV_RS traffic, no
 * clock poke — so the per-track call in hal_audio_init can never reset the
 * controller out from under a codec write. A pending transaction is waited
 * out here too: BUSY twice, then idle. */
static int test_i2c_reinit_is_idempotent(void)
{
    static const uint32_t seq[] = { I2C_BUSY, I2C_BUSY, 0 };
    i2c_test_reset();
    mmio_mock_reset();
    mmio_mock_set_read(DEV_EN_ADDR,     0);
    mmio_mock_set_read(DEV_RS_ADDR,     0);
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    i2c_init();                                    /* first: full sequence */

    mmio_mock_reset();
    mmio_mock_queue_read(I2C_STATUS_ADDR, seq, 3);
    i2c_init();                                    /* second: drain only   */

    trace_cursor tc = trace_begin("i2c_reinit_idempotent");
    for (int i = 0; i < 3; i++) {
        expect_r(&tc, 8, I2C_STATUS_ADDR);         /* BUSY x2, then idle */
    }
    trace_expect_end(&tc);
    int fails = trace_done(&tc);
    fails += check("i2c_init again: no DEV_RS write",
                   mmio_mock_count(MMIO_OP_WRITE, DEV_RS_ADDR) == 0);
    fails += check("i2c_init again: no DEV_EN write",
                   mmio_mock_count(MMIO_OP_WRITE, DEV_EN_ADDR) == 0);
    fails += check("i2c_init again: no clock poke",
                   mmio_mock_count(MMIO_OP_WRITE, I2C_CLKCFG_ADDR) == 0);

    /* A third call is the same drain. */
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    i2c_init();
    fails += check("i2c_init a third time: one idle poll, nothing else",
                   mmio_mock_log_len() == 1 &&
                   mmio_mock_count(MMIO_OP_READ, I2C_STATUS_ADDR) == 1);
    return fails;
}

/* Case 1c: a re-init whose idle wait TIMES OUT is a wedged controller (BUSY
 * stuck), not a transaction to protect. Nothing good is in flight, and
 * without a reset every later write would time out for the rest of the
 * boot — the codec, the PMU standby command, the battery ADC all ride this
 * bus. So the idempotent path falls through to the full reset sequence. */
static int test_i2c_reinit_recovers_a_stuck_controller(void)
{
    i2c_test_reset();
    mmio_mock_reset();
    mmio_mock_set_read(DEV_EN_ADDR,     0);
    mmio_mock_set_read(DEV_RS_ADDR,     0);
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    i2c_init();                                    /* first: full sequence */

    mmio_mock_reset();
    mmio_mock_set_read(DEV_EN_ADDR,     0);
    mmio_mock_set_read(DEV_RS_ADDR,     0);
    mmio_mock_set_read(I2C_STATUS_ADDR, I2C_BUSY); /* stuck for good       */
    i2c_init();
    /* 65536 BUSY polls saturate the mock's log, so the reset writes are not
     * recorded — count EVENTS instead: the idempotent early return is exactly
     * one full wait; falling through to the reset sequence adds its own
     * end-of-init wait, so a stuck bus costs at least two. */
    size_t total = mmio_mock_log_len() + mmio_mock_dropped();
    int fails = 0;
    fails += check("i2c_init on a stuck bus: falls through to the reset "
                   "(two full waits, not one)",
                   total >= 2u * (1u << 16));
    return fails;
}

/* Case 2: i2c_send emits the exact controller grammar for a 2-byte
 * write to device 0x1a. CTRL reads 0, STATUS reads idle. */
static int test_i2c_send_grammar(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    mmio_mock_set_read(I2C_CTRL_ADDR,   0);

    const uint8_t payload[2] = { 0xAB, 0xCD };
    int rc = i2c_send(0x1A, payload, 2);

    int fails = check("i2c_send: returns 0", rc == 0);
    trace_cursor tc = trace_begin("i2c_send");
    expect_r(&tc, 8, I2C_STATUS_ADDR);              /* wait idle */
    expect_w(&tc, 8, I2C_ADDR_ADDR, 0x34);          /* 0x1a<<1, write */
    expect_r(&tc, 8, I2C_CTRL_ADDR);
    expect_w(&tc, 8, I2C_CTRL_ADDR, 0x00);          /* clear read bit */
    expect_w(&tc, 8, I2C_DATA0_ADDR, 0xAB);
    expect_w(&tc, 8, I2C_DATA1_ADDR, 0xCD);
    expect_r(&tc, 8, I2C_CTRL_ADDR);
    expect_w(&tc, 8, I2C_CTRL_ADDR, 0x02);          /* count = (2-1)<<1 */
    expect_r(&tc, 8, I2C_CTRL_ADDR);
    expect_w(&tc, 8, I2C_CTRL_ADDR, I2C_SEND);      /* strobe */
    trace_expect_end(&tc);
    fails += trace_done(&tc);
    return fails;
}

/* Case 3: bad lengths are rejected with no bus traffic; a BUSY-forever
 * bus times out (bounded) and never loads an address. */
static int test_i2c_send_guards(void)
{
    int fails = 0;
    const uint8_t buf[5] = { 0 };

    mmio_mock_reset();
    fails += check("i2c_send len=0 -> -1", i2c_send(0x1A, buf, 0) == -1);
    fails += check("i2c_send len=5 -> -1", i2c_send(0x1A, buf, 5) == -1);
    fails += check("rejects: zero bus traffic", mmio_mock_log_len() == 0);

    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, I2C_BUSY);  /* busy forever */
    fails += check("i2c_send BUSY -> -2", i2c_send(0x1A, buf, 2) == -2);
    fails += check("BUSY: bounded (many STATUS reads)",
                   count_writes(I2C_ADDR_ADDR) == 0 &&
                   mmio_mock_count(MMIO_OP_READ, I2C_STATUS_ADDR) > 1);
    return fails;
}

/* Log index of the first codec write carrying (b0, b1) — the DATA0 write's
 * index, its DATA1 partner being the next DATA1 write; -1 if none. Lets a
 * case order codec writes against each other and against the timer waits. */
static long codec_write_index(uint8_t b0, uint8_t b1)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    for (size_t i = 0; i < len; i++) {
        if (log[i].op != MMIO_OP_WRITE || log[i].addr != I2C_DATA0_ADDR ||
            log[i].value != b0) {
            continue;
        }
        for (size_t j = i + 1; j < len; j++) {
            if (log[j].op == MMIO_OP_WRITE && log[j].addr == I2C_DATA1_ADDR) {
                if (log[j].value == b1) {
                    return (long)i;
                }
                break;
            }
        }
    }
    return -1;
}

/* Log index of the first read of `addr`; -1 if none. */
static long first_read_index(uint32_t addr)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    for (size_t i = 0; i < len; i++) {
        if (log[i].op == MMIO_OP_READ && log[i].addr == addr) {
            return (long)i;
        }
    }
    return -1;
}

/* Codec payloads (reg<<1 | data bit 8, data low byte) the sequencing cases
 * look for. Hand-packed from wm8758.h's register numbers and bit values. */
#define CW_RESET_B0          0x00
#define CW_PWRMGMT1_B0       0x02
#define CW_PWRMGMT1_RUN_B1   0x2D   /* PLLEN|BIASEN|BUFIOEN|VMIDSEL_75K       */
#define CW_PWRMGMT1_10K_B1   0x2F   /* ...|VMIDSEL_10K: the old fast charge   */
#define CW_PWRMGMT1_PLLOFF_B1 0x0D  /* BIASEN|BUFIOEN|VMIDSEL_75K (PLLEN off) */
#define CW_PWRMGMT1_DRAIN_B1 0x28   /* PLLEN|BIASEN: VMID divider + BUFIO off */
#define CW_PWRMGMT2_B0       0x04
#define CW_PWRMGMT3_B0       0x06
#define CW_DACCTRL_B0        0x14
#define CW_DACCTRL_MUTED_B1  0x48   /* DACOSR128 | SOFTMUTE                   */
#define CW_DACCTRL_UNMUTE_B1 0x08   /* DACOSR128 alone                        */
#define CW_PLLN_B0           0x48
#define CW_PLLN_44_B1        0x17
#define CW_PLLN_48_B1        0x18
#define CW_ADDCTRL_B0        0x0E
#define CW_OUT4TOADC_B0      0x54
#define CW_OUT4TOADC_POB_B1  0x04   /* POBCTRL                                */
#define CW_OUT4TOADC_DRAIN_B1 0x14  /* POBCTRL | VMIDTOG                      */
#define CW_OUTCTRL_B0        0x63   /* OUTCTRL 0x31, bit 8 (HP_COM) set       */
#define CW_OUTCTRL_NOTSD_B1  0x85   /* HP_COM|LINE_COM|TSOPCTRL|VROI, no TSDEN*/
#define CW_BIASCTRL_B0       0x7A   /* BIASCTRL 0x3D, bit 8 clear (= 0)       */
#define CW_LOUT1VOL_B0       0x68
#define CW_LOUT1VOL_0DB_B1   0xB9   /* 0x39 | ZC                              */
#define CW_ROUT1VOL_B0       0x6B   /* ROUT1VOL 0x35 with VU (bit 8)          */

/* Case 4: wm8758_init is the datasheet's power-up sequence. 30 codec
 * writes; correct 9-bit packing on the first (BIASCTRL) and the 44.1k
 * CLKCTRL; the DAC->mixer route present; VMID charged on the 75k divider
 * (never the 10k fast one); the VMID rise waited out for 100 ms AFTER the
 * charge starts and BEFORE the outputs are unmuted; POBCTRL held through
 * that wait and dropped as the LAST write; the DAC left soft-muted. */
static int test_wm8758_init(void)
{
    int fails = 0;
    /* USEC_TIMER script: t0, then 40 ms (the OLD settle — a driver still
     * waiting 40 ms exits here), 99 999 us (still short), 100 000 (done).
     * Every read after the last repeats it. Exactly four reads means the
     * wait is precisely 100 ms; two would mean 40. */
    static const uint32_t rise[] = { 0u, 40000u, 99999u, 100000u };
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    mmio_mock_set_read(I2C_CTRL_ADDR,   0);
    mmio_mock_queue_read(USEC_TIMER_ADDR, rise, 4);

    wm8758_init();

    /* One I2C_ADDR write per codec register write (30 = reset + 29). */
    fails += check("wm8758_init: 30 codec writes",
                   count_writes(I2C_ADDR_ADDR) == 30);
    /* Every transaction addresses the codec (0x1a<<1). */
    fails += check("wm8758_init: all addressed to 0x34",
                   nth_write(I2C_ADDR_ADDR, 0) == 0x34 &&
                   nth_write(I2C_ADDR_ADDR, 29) == 0x34);
    /* First write is the soft RESET(0x00)=0: b0 = 0x00, b1 = 0x00. */
    fails += check("wm8758_init: first write is soft RESET (0x00/0x00)",
                   nth_write(I2C_DATA0_ADDR, 0) == 0x00 &&
                   nth_write(I2C_DATA1_ADDR, 0) == 0x00);
    /* BIASCTRL(0x3d)=BIASCUT(0x100) still present: b0 = 0x7B, b1 = 0x00. */
    fails += check("wm8758_init: BIASCTRL preinit present (0x7B/0x00)",
                   has_codec_write(0x7B, 0x00));
    /* 44.1k CLKCTRL(0x06)=0x145: b0 = 0x0D, b1 = 0x45. */
    fails += check("wm8758_init: 44.1k CLKCTRL 0x145 present (0x0D/0x45)",
                   has_codec_write(0x0D, 0x45));
    /* DAC->left mixer route LOUTMIX(0x32)=0x001: b0 = 0x64, b1 = 0x01.
     * Without this the DAC is powered but unrouted -> silence. */
    fails += check("wm8758_init: DAC->mixer route present (0x64/0x01)",
                   has_codec_write(0x64, 0x01));
    /* DACCTRL(0x0a) = DACOSR128|SOFTMUTE (0x48) is written, and the bring-up
     * ENDS MUTED — nothing feeds the serializer yet, and an unmuted DAC over
     * an idle serializer plays its underrun output; hal_audio_start unmutes
     * after the DMA kick. No write in the whole bring-up ever unmutes. */
    fails += check("wm8758_init: DACCTRL = OSR128 | SOFTMUTE is written",
                   has_codec_write(CW_DACCTRL_B0, CW_DACCTRL_MUTED_B1));
    fails += check("wm8758_init: never unmutes the DAC",
                   !has_codec_write(CW_DACCTRL_B0, CW_DACCTRL_UNMUTE_B1));

    /* --- the datasheet's sequencing ------------------------------------ */
    long vmid_on  = codec_write_index(CW_PWRMGMT1_B0, CW_PWRMGMT1_RUN_B1);
    long pob_on   = codec_write_index(CW_OUT4TOADC_B0, CW_OUT4TOADC_POB_B1);
    long route    = codec_write_index(0x64, 0x01);
    long wait     = first_read_index(USEC_TIMER_ADDR);
    long bias_ok  = codec_write_index(CW_BIASCTRL_B0, 0x00);
    long unmute_l = codec_write_index(CW_LOUT1VOL_B0, CW_LOUT1VOL_0DB_B1);
    long unmute_r = codec_write_index(CW_ROUT1VOL_B0, CW_LOUT1VOL_0DB_B1);
    long dacmute  = codec_write_index(CW_DACCTRL_B0, CW_DACCTRL_MUTED_B1);
    long pob_off  = codec_write_index(CW_OUT4TOADC_B0, 0x00);

    /* VMID charges on the divider it plays through. The 10k fast charge was
     * a per-track-bring-up compromise, and a 7x steeper thump. */
    fails += check("wm8758_init: VMID charged on the 75k divider (PWRMGMT1 = 0x2D)",
                   vmid_on >= 0);
    fails += check("wm8758_init: never the 10k fast charge (no PWRMGMT1 = 0x2F)",
                   !has_codec_write(CW_PWRMGMT1_B0, CW_PWRMGMT1_10K_B1));
    /* POBCTRL is on before VMID starts charging, and the amps are already
     * enabled by then (PWRMGMT2 precedes PWRMGMT1 in the table). */
    fails += check("wm8758_init: POBCTRL set BEFORE VMID starts charging",
                   pob_on >= 0 && vmid_on >= 0 && pob_on < vmid_on);
    /* The rise is waited out AFTER the charge starts and the interface, PLL
     * and route are programmed, and BEFORE anything is unmuted. */
    fails += check("wm8758_init: the VMID wait follows the charge start and the DAC route",
                   wait >= 0 && wait > vmid_on && wait > route);
    fails += check("wm8758_init: the VMID wait is 100 ms (exactly four timer reads)",
                   mmio_mock_count(MMIO_OP_READ, USEC_TIMER_ADDR) == 4);
    fails += check("wm8758_init: low-bias cleared and outputs unmuted only AFTER the wait",
                   bias_ok > wait && unmute_l > wait && unmute_r > wait &&
                   dacmute > wait);
    /* POBCTRL comes off LAST — after the output unmute, after DACCTRL. It
     * used to come off before the wait even began. */
    fails += check("wm8758_init: POBCTRL dropped AFTER the output unmute",
                   pob_off >= 0 && pob_off > unmute_r && pob_off > dacmute);
    fails += check("wm8758_init: OUT4TOADC = 0 (POBCTRL off) is the LAST codec write",
                   nth_write(I2C_DATA0_ADDR, 29) == CW_OUT4TOADC_B0 &&
                   nth_write(I2C_DATA1_ADDR, 29) == 0x00);
    return fails;
}

/* Case 4b: wm8758_powerdown is the datasheet's power-down sequence: DAC and
 * outputs muted, thermal shutdown off, VMID discharge begun (POBCTRL |
 * VMIDTOG) and the VMID divider + I/O buffer dropped, THEN a 300 ms drain,
 * and only after it the amps, bias/PLL and DAC/mixers off. The drain used
 * to be missing: three back-to-back writes cut the amps with VMID at
 * midrail. */
static int test_wm8758_powerdown(void)
{
    int fails = 0;
    /* t0, 150 ms (an under-length drain exits here), 299 999, 300 000. */
    static const uint32_t drain[] = { 0u, 150000u, 299999u, 300000u };
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    mmio_mock_set_read(I2C_CTRL_ADDR,   0);
    mmio_mock_queue_read(USEC_TIMER_ADDR, drain, 4);

    wm8758_powerdown();

    long dacmute = codec_write_index(CW_DACCTRL_B0, CW_DACCTRL_MUTED_B1);
    long no_tsd  = codec_write_index(CW_OUTCTRL_B0, CW_OUTCTRL_NOTSD_B1);
    long vmidtog = codec_write_index(CW_OUT4TOADC_B0, CW_OUT4TOADC_DRAIN_B1);
    long divoff  = codec_write_index(CW_PWRMGMT1_B0, CW_PWRMGMT1_DRAIN_B1);
    long wait    = first_read_index(USEC_TIMER_ADDR);
    long r1_off  = codec_write_index(CW_PWRMGMT1_B0, 0x00);
    long r2_off  = codec_write_index(CW_PWRMGMT2_B0, 0x00);
    long r3_off  = codec_write_index(CW_PWRMGMT3_B0, 0x00);

    fails += check("wm8758_powerdown: 9 codec writes",
                   count_writes(I2C_ADDR_ADDR) == 9);
    fails += check("wm8758_powerdown: DAC soft-muted first",
                   dacmute == codec_write_index(CW_DACCTRL_B0, CW_DACCTRL_MUTED_B1) &&
                   dacmute >= 0 && dacmute < no_tsd);
    fails += check("wm8758_powerdown: thermal shutdown off, then VMIDTOG, then the divider off",
                   no_tsd >= 0 && vmidtog >= 0 && divoff >= 0 &&
                   no_tsd < vmidtog && vmidtog < divoff);
    fails += check("wm8758_powerdown: the drain wait follows the divider-off write",
                   wait >= 0 && wait > divoff);
    fails += check("wm8758_powerdown: the drain is 300 ms (exactly four timer reads)",
                   mmio_mock_count(MMIO_OP_READ, USEC_TIMER_ADDR) == 4);
    fails += check("wm8758_powerdown: amps, bias/PLL and DAC/mixers off only AFTER the drain",
                   r1_off > wait && r2_off > wait && r3_off > wait);
    fails += check("wm8758_powerdown: never a WM_RESET",
                   !has_codec_write(CW_RESET_B0, 0x00));
    return fails;
}

/* Case 4c: wm8758_retune, the warm track change. At the rate the codec is
 * already programmed for it writes nothing; at a new rate it re-programs
 * the PLL and dividers with PLLEN cleared across the write and restored
 * after, then waits for lock — and never resets, never touches the rails,
 * never touches DACCTRL. Runs after case 4 left the codec at 44.1 kHz. */
static int test_wm8758_retune(void)
{
    int fails = 0;

    /* Case 4b left the codec COLD (a power-down forgets what was
     * programmed, correctly — the next bring-up is a reset). Warm it at
     * 44.1 kHz first; a retune is only defined against a warm codec. */
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    mmio_mock_set_read(I2C_CTRL_ADDR,   0);
    (void)wm8758_set_rate(44100u);
    (void)wm8758_init();

    /* Same rate: silence on the bus. */
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    mmio_mock_set_read(I2C_CTRL_ADDR,   0);
    fails += check("wm8758_retune: 44.1k -> 44.1k accepted", wm8758_set_rate(44100u) == 0);
    fails += check("wm8758_retune: returns 0 at an unchanged rate", wm8758_retune() == 0);
    fails += check("wm8758_retune: an unchanged rate writes NOTHING",
                   mmio_mock_log_len() == 0);

    /* New rate: PLLEN off, six rate writes, PLLEN on, lock wait. */
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    mmio_mock_set_read(I2C_CTRL_ADDR,   0);
    fails += check("wm8758_retune: 48k accepted", wm8758_set_rate(48000u) == 0);
    fails += check("wm8758_retune: returns 0 at 48k", wm8758_retune() == 0);

    long plloff = codec_write_index(CW_PWRMGMT1_B0, CW_PWRMGMT1_PLLOFF_B1);
    long plln   = codec_write_index(CW_PLLN_B0, CW_PLLN_48_B1);
    long addctl = codec_write_index(CW_ADDCTRL_B0, 0x01);
    long pllon  = codec_write_index(CW_PWRMGMT1_B0, CW_PWRMGMT1_RUN_B1);
    long wait   = first_read_index(USEC_TIMER_ADDR);
    fails += check("wm8758_retune: 8 codec writes for a rate change",
                   count_writes(I2C_ADDR_ADDR) == 8);
    fails += check("wm8758_retune: PLLEN off BEFORE the PLL is re-programmed",
                   plloff >= 0 && plln >= 0 && plloff < plln);
    fails += check("wm8758_retune: PLLEN back on AFTER the last rate register",
                   addctl >= 0 && pllon >= 0 && addctl < pllon && plln < addctl);
    fails += check("wm8758_retune: the lock wait follows PLLEN on",
                   wait >= 0 && wait > pllon);
    fails += check("wm8758_retune: the 48k CLKCTRL 0x145 is present",
                   has_codec_write(0x0D, 0x45));
    fails += check("wm8758_retune: no WM_RESET, no rails, no DACCTRL, no POBCTRL",
                   !has_codec_write(CW_RESET_B0, 0x00) &&
                   !has_codec_write(CW_PWRMGMT1_B0, 0x00) &&
                   !has_codec_write(CW_PWRMGMT2_B0, 0x00) &&
                   !has_codec_write(CW_DACCTRL_B0, CW_DACCTRL_MUTED_B1) &&
                   !has_codec_write(CW_DACCTRL_B0, CW_DACCTRL_UNMUTE_B1) &&
                   !has_codec_write(CW_OUT4TOADC_B0, 0x00) &&
                   !has_codec_write(CW_OUT4TOADC_B0, CW_OUT4TOADC_DRAIN_B1));

    /* And back: the change is what is written, not the rate itself. */
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    mmio_mock_set_read(I2C_CTRL_ADDR,   0);
    (void)wm8758_set_rate(44100u);
    (void)wm8758_retune();
    fails += check("wm8758_retune: 48k -> 44.1k re-programs (PLLN 0x17 present)",
                   has_codec_write(CW_PLLN_B0, CW_PLLN_44_B1) &&
                   count_writes(I2C_ADDR_ADDR) == 8);
    return fails;
}

/* Case 5: i2s_init clock-plumbing + FIFO reset/format grammar. All RMW
 * sources read 0, so each |= /&= resolves to a fixed value. */
static int test_i2s_init_grammar(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(DEV_RS_ADDR,           0);
    mmio_mock_set_read(DEV_EN_ADDR,           0);
    mmio_mock_set_read(DEV_EXTCLK_SEL_ADDR,   0);
    mmio_mock_set_read(DEV_INIT2_ADDR,        0);
    mmio_mock_set_read(DEV_INIT1_ADDR,        0);
    mmio_mock_set_read(IISCONFIG_ADDR,        0);
    mmio_mock_set_read(IISFIFO_CFG_ADDR,      0);

    i2s_init();

    trace_cursor tc = trace_begin("i2s_init");
    /* clock plumbing */
    expect_r(&tc, 32, DEV_RS_ADDR);
    expect_w(&tc, 32, DEV_RS_ADDR, DEV_I2S);
    expect_r(&tc, 32, DEV_RS_ADDR);
    expect_w(&tc, 32, DEV_RS_ADDR, 0);
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, DEV_I2S);
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, DEV_EXTCLOCKS);
    expect_r(&tc, 32, DEV_EXTCLK_SEL_ADDR);
    expect_w(&tc, 32, DEV_EXTCLK_SEL_ADDR, 0);
    /* I2S/CDI pad-function select (clear -> I2S alt function) */
    expect_r(&tc, 32, DEV_INIT2_ADDR);
    expect_w(&tc, 32, DEV_INIT2_ADDR, 0);
    expect_r(&tc, 32, DEV_INIT1_ADDR);
    expect_w(&tc, 32, DEV_INIT1_ADDR, 0);
    /* FIFO reset pulse */
    expect_r(&tc, 32, IISCONFIG_ADDR);
    expect_w(&tc, 32, IISCONFIG_ADDR, IIS_RESET);
    expect_r(&tc, 32, IISCONFIG_ADDR);
    expect_w(&tc, 32, IISCONFIG_ADDR, 0);
    /* format: FORMAT_IIS(0), SIZE_16BIT(0), FIFO_FORMAT_LE16_2(0x70) */
    expect_r(&tc, 32, IISCONFIG_ADDR);
    expect_w(&tc, 32, IISCONFIG_ADDR, IIS_FORMAT_IIS);
    expect_r(&tc, 32, IISCONFIG_ADDR);
    expect_w(&tc, 32, IISCONFIG_ADDR, IIS_SIZE_16BIT);
    expect_r(&tc, 32, IISCONFIG_ADDR);
    expect_w(&tc, 32, IISCONFIG_ADDR, IIS_FIFO_FORMAT_LE16_2);
    /* FIFO attention levels, then flush */
    expect_r(&tc, 32, IISFIFO_CFG_ADDR);
    expect_w(&tc, 32, IISFIFO_CFG_ADDR, IIS_RX_FULL_LVL_12 | IIS_TX_EMPTY_LVL_4);
    expect_r(&tc, 32, IISFIFO_CFG_ADDR);
    expect_w(&tc, 32, IISFIFO_CFG_ADDR, IIS_RXCLR | IIS_TXCLR);
    trace_expect_end(&tc);
    return trace_done(&tc);
}

/* Case 6: i2s_write_stereo polls TXFree, then writes one packed word;
 * left in the low 16 bits, right in the high 16. */
static int test_i2s_write(void)
{
    int fails = 0;
    mmio_mock_reset();
    mmio_mock_set_read(IISFIFO_CFG_ADDR, 4u << IISFIFO_CFG_TXFREE_SHIFT); /* 4 free */

    int rc = i2s_write_stereo((int16_t)0x1234, (int16_t)0x5678);

    fails += check("i2s_write_stereo: returns 0", rc == 0);
    trace_cursor tc = trace_begin("i2s_write");
    expect_r(&tc, 32, IISFIFO_CFG_ADDR);
    expect_w(&tc, 32, IISFIFO_WR_ADDR, 0x56781234u);   /* R<<16 | L */
    trace_expect_end(&tc);
    fails += trace_done(&tc);
    return fails;
}

/* Case 7: a never-draining TX FIFO (TXFree stuck at 0) returns -1 after
 * a bounded spin and never writes a sample. */
static int test_i2s_write_noclock(void)
{
    int fails = 0;
    mmio_mock_reset();
    mmio_mock_set_read(IISFIFO_CFG_ADDR, 0);   /* TXFree == 0 forever */

    int rc = i2s_write_stereo(1, 1);

    fails += check("i2s_write_stereo: no-drain -> -1", rc == -1);
    fails += check("no-drain: bounded, wrote no sample",
                   count_writes(IISFIFO_WR_ADDR) == 0 &&
                   mmio_mock_count(MMIO_OP_READ, IISFIFO_CFG_ADDR) > 1);
    return fails;
}

/* Case 8: dma_playback_init grammar — enable master + IIS request line,
 * static channel config (dest = I2S FIFO fixed 32-bit), clear latched. */
static int test_dma_init_grammar(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(CPU_INT_PRIORITY_ADDR,   0);
    mmio_mock_set_read(DMA_MASTER_CONTROL_ADDR, 0);
    mmio_mock_set_read(DMA_REQ_STATUS_ADDR,     0);
    mmio_mock_set_read(DMA0_STATUS_ADDR,        0);

    dma_playback_init();

    trace_cursor tc = trace_begin("dma_init");
    /* force IRQ (not FIQ) routing for source 26 */
    expect_r(&tc, 32, CPU_INT_PRIORITY_ADDR);
    expect_w(&tc, 32, CPU_INT_PRIORITY_ADDR, 0);   /* 0 & ~DMA_MASK */
    expect_r(&tc, 32, DMA_MASTER_CONTROL_ADDR);
    expect_w(&tc, 32, DMA_MASTER_CONTROL_ADDR, DMA_MASTER_CONTROL_EN);
    expect_r(&tc, 32, DMA_REQ_STATUS_ADDR);
    expect_w(&tc, 32, DMA_REQ_STATUS_ADDR, 1u << DMA_REQ_IIS);   /* 0x4 */
    expect_w(&tc, 32, DMA0_PER_ADDR_ADDR, IISFIFO_WR_ADDR);      /* 0x70002840 */
    expect_w(&tc, 32, DMA0_FLAGS_ADDR, DMA_FLAGS_PLAY);          /* 0x04000000 */
    expect_w(&tc, 32, DMA0_INCR_ADDR, DMA_INCR_PLAY);            /* 0x20010000 */
    expect_r(&tc, 32, DMA0_STATUS_ADDR);                         /* clear latched */
    trace_expect_end(&tc);
    return trace_done(&tc);
}

/* Case 9: dma_playback_kick programs RAM address then the command word
 * with SIZE = bytes-4 and START. 8192 bytes -> CMD 0xCD021FFC. */
static int test_dma_kick_grammar(void)
{
    mmio_mock_reset();

    dma_playback_kick(0x10004000u, 8192u);

    trace_cursor tc = trace_begin("dma_kick");
    expect_w(&tc, 32, DMA0_RAM_ADDR_ADDR, 0x10004000u);
    /* 0x4D020000 | (8192-4=0x1FFC) | 0x80000000 = 0xCD021FFC */
    expect_w(&tc, 32, DMA0_CMD_ADDR,
             DMA_PLAY_CONFIG | ((8192u - DMA_SIZE_BIAS) & DMA_CMD_SIZE_MASK)
                 | DMA_CMD_START);
    trace_expect_end(&tc);
    return trace_done(&tc);
}

/* Case 10: ack is a bare status read (clears the IRQ); stop clears
 * START/INTR and, with STATUS idle, exits its bounded wait immediately. */
static int test_dma_ack_stop(void)
{
    int fails = 0;

    mmio_mock_reset();
    dma_playback_ack();
    fails += check("dma_ack: single status read",
                   mmio_mock_log_len() == 1 &&
                   mmio_mock_count(MMIO_OP_READ, DMA0_STATUS_ADDR) == 1);

    mmio_mock_reset();
    mmio_mock_set_read(DMA0_CMD_ADDR,    0);
    mmio_mock_set_read(DMA0_STATUS_ADDR, 0);   /* idle */
    dma_playback_stop();
    trace_cursor tc = trace_begin("dma_stop");
    expect_r(&tc, 32, DMA0_CMD_ADDR);
    expect_w(&tc, 32, DMA0_CMD_ADDR, 0);       /* START|INTR cleared from 0 */
    expect_r(&tc, 32, DMA0_STATUS_ADDR);       /* not busy -> one poll */
    trace_expect_end(&tc);
    fails += trace_done(&tc);
    return fails;
}

int main(void)
{
    int fails = 0;
    fails += test_i2c_init_grammar();
    fails += test_i2c_init_waits_before_reset();
    fails += test_i2c_reinit_is_idempotent();
    fails += test_i2c_reinit_recovers_a_stuck_controller();
    fails += test_i2c_send_grammar();
    fails += test_i2c_send_guards();
    fails += test_wm8758_init();
    fails += test_wm8758_powerdown();
    fails += test_wm8758_retune();
    fails += test_i2s_init_grammar();
    fails += test_i2s_write();
    fails += test_i2s_write_noclock();
    fails += test_dma_init_grammar();
    fails += test_dma_kick_grammar();
    fails += test_dma_ack_stop();

    if (fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", fails, fails == 1 ? "" : "s");
    }
    return fails == 0 ? 0 : 1;
}
