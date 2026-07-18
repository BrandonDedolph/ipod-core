/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fs/fat32_test.c — host test for the read-only FAT32 reader.
 *
 * Mounts the synthetic image built by tests/scripts/make_fat32_image.py
 * (BytesPerSector 2048, so sec_ratio = 4; a file spanning two clusters)
 * through a file-backed 512-byte block callback, then checks mount, the
 * 8.3 lookup (case-insensitive, hit + miss), and the file read (full and
 * partial). The image path is argv[1] (passed by meson).
 */

#include "fat32.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static FILE *g_img;

/* 512-byte block read from the image file. part_lba is 0 (the image is the
 * volume itself, no MBR), so `lba` is a plain FS offset in 512-byte units. */
static int img_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    if (fseek(g_img, (long)lba * 512L, SEEK_SET) != 0) {
        return -1;
    }
    if (fread(buf, 512, count, g_img) != count) {
        return -1;
    }
    return 0;
}

static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    return cond ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <fat32-image>\n", argv[0]);
        return 2;
    }
    g_img = fopen(argv[1], "rb");
    if (!g_img) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    int fails = 0;
    fat32_t fs;

    fails += check("mount returns 0", fat32_mount(&fs, img_read, NULL, 0) == 0);
    fails += check("BytesPerSec = 2048", fs.bytes_per_sec == 2048);
    fails += check("sec_ratio = 4", fs.sec_ratio == 4);
    fails += check("root cluster = 2", fs.root_clus == 2);

    uint32_t clus = 0, size = 0;

    /* exact-name lookup */
    fails += check("open HELLO.TXT returns 0",
                   fat32_open(&fs, "HELLO.TXT", &clus, &size) == 0);
    fails += check("HELLO.TXT first cluster = 3", clus == 3);
    fails += check("HELLO.TXT size = 3000", size == 3000);

    /* case-insensitive */
    uint32_t c2 = 0, s2 = 0;
    fails += check("open hello.txt (lowercase) returns 0",
                   fat32_open(&fs, "hello.txt", &c2, &s2) == 0 &&
                   c2 == 3 && s2 == 3000);

    /* miss */
    uint32_t c3, s3;
    fails += check("open NOPE.TXT returns -1",
                   fat32_open(&fs, "NOPE.TXT", &c3, &s3) == -1);

    /* full read: 3000 bytes of the i&0xFF pattern, spanning 2 clusters */
    static uint8_t buf[8192];
    int32_t n = fat32_read_file(&fs, clus, buf, size);
    fails += check("read_file returns 3000", n == 3000);
    int content_ok = 1;
    for (int i = 0; i < 3000; i++) {
        if (buf[i] != (uint8_t)(i & 0xFF)) {
            content_ok = 0;
            printf("  content mismatch at %d: got %02X want %02X\n",
                   i, buf[i], (uint8_t)(i & 0xFF));
            break;
        }
    }
    fails += check("read_file content matches pattern (2-cluster span)",
                   content_ok);

    /* partial read: first 100 bytes only */
    memset(buf, 0xCC, sizeof buf);
    int32_t p = fat32_read_file(&fs, clus, buf, 100);
    int partial_ok = (p == 100);
    for (int i = 0; i < 100 && partial_ok; i++) {
        partial_ok = buf[i] == (uint8_t)(i & 0xFF);
    }
    fails += check("partial read (maxlen=100) returns 100 correct bytes",
                   partial_ok);

    fclose(g_img);
    if (fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", fails, fails == 1 ? "" : "s");
    }
    return fails == 0 ? 0 : 1;
}
