/* Real BGA clock regression: queued audio is not presented audio. */
#include "ps2_bga_clock.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    ps2_bga_clock c = {0};
    ps2_bga_clock_seek(&c, 0);
    ps2_bga_clock_submit(&c, 10000, 256);
    ps2_bga_clock_submit(&c, 10256, 256);
    assert(c.submitted == 512);
    assert(ps2_bga_clock_position(&c, 9000) == 0);
    assert(ps2_bga_clock_position(&c, 10100) == 100);
    assert(ps2_bga_clock_position(&c, 10300) == 300);
    /* A two-second pause freezes after the queued tail is heard. */
    ps2_bga_clock_submit(&c, 98712, 256);
    assert(ps2_bga_clock_position(&c, 40000) == 512);
    assert(ps2_bga_clock_position(&c, 98712) == 512);
    assert(ps2_bga_clock_position(&c, 98812) == 612);
    /* Seek/restart discards the old generation even if its tail is queued. */
    ps2_bga_clock_seek(&c, 44100);
    ps2_bga_clock_submit(&c, 100000, 256);
    assert(ps2_bga_clock_position(&c, 99000) == 44100);
    assert(ps2_bga_clock_position(&c, 100128) == 44228);
    ps2_bga_clock_seek(&c, 0);
    for (unsigned i = 0; i < 200000; i++) {
        ps2_bga_clock_submit(&c, 200000ull + i * 256ull, 256);
        if (i > 20)
            assert(ps2_bga_clock_position(&c, 200000ull + (i - 20ull) * 256 + 77) == (i - 20ull) * 256 + 77);
    }
    assert(c.count == BGA_CLOCK_BLOCKS);
    puts("OK: output latency, pause/resume, seek/restart and 19-minute ring wrap");
    return 0;
}
