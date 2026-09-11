/* Headless demo for purecat: load CAT.EXE, run a while, write the screen out as a PPM.
 *
 *     purecat_demo <CAT.EXE> [instructions] [out.ppm]
 *
 * The point of it is verification. Running the same instruction count as tools/emu8086.py must
 * give a pixel-identical screen; if it does, this C port and the Python reference agree.
 */
#include "purealleycat.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "../original/CAT.EXE";
    long steps = argc > 2 ? strtol(argv[2], NULL, 10) : 2000000;
    const char *out = argc > 3 ? argv[3] : "screen.ppm";

    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return 1; }
    static unsigned char exe[1 << 20];
    int size = (int)fread(exe, 1, sizeof exe, file);
    fclose(file);

    if (purecat_init(exe, size) != 0) {
        fprintf(stderr, "%s: not an MZ executable\n", path);
        return 1;
    }
    printf("loaded %d bytes\n", size);

    purecat_run((int)steps);

    printf("instructions: %llu\n", (unsigned long long)purecat_instructions());
    printf("status: %s\n", purecat_status());
    if (purecat_fault_opcode() >= 0)
        printf("fault: opcode %02X at %04X:%04X\n", purecat_fault_opcode(),
               purecat_fault_address() >> 16, purecat_fault_address() & 0xFFFF);
    printf("video mode set: %s\n", purecat_ready() ? "yes" : "no");

    const unsigned char *frame = purecat_framebuffer();
    unsigned int palette[4];
    purecat_palette(palette);

    int painted = 0;
    for (int i = 0; i < PURECAT_WIDTH * PURECAT_HEIGHT; i++)
        if (frame[i]) painted++;
    printf("non-background pixels: %d of %d\n", painted, PURECAT_WIDTH * PURECAT_HEIGHT);

    FILE *ppm = fopen(out, "wb");
    if (!ppm) { perror(out); return 1; }
    fprintf(ppm, "P6\n%d %d\n255\n", PURECAT_WIDTH, PURECAT_HEIGHT);
    for (int i = 0; i < PURECAT_WIDTH * PURECAT_HEIGHT; i++) {
        unsigned int rgb = palette[frame[i]];
        fputc((int)((rgb >> 16) & 0xFF), ppm);
        fputc((int)((rgb >> 8) & 0xFF), ppm);
        fputc((int)(rgb & 0xFF), ppm);
    }
    fclose(ppm);
    printf("wrote %s\n", out);
    return 0;
}
