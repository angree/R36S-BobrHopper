// The 68060 build's 16.16 arithmetic (src/engine/fixed.h, CR_AMIGA_060) against the 64-bit code it replaces, bit for
// bit: the 060 has neither a 32x32->64 multiply nor a 64/32 divide, so that build does both with 32-bit instructions,
// and it must not change a single result - the game's logic has to play out identically on every Amiga.
//   test_fixed060.exe
#define CR_AMIGA_060 1
#include <cstdint>
#include <cstdio>

#include "engine/fixed.h"

using namespace cr;

static uint32_t s = 2463534242u;
static uint32_t rnd()
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
// the values that matter most: small ones, round ones, the edges of the range
static int32_t pick()
{
    const uint32_t r = rnd();
    switch (r & 7) {
    case 0: return int32_t(rnd());
    case 1: return int32_t(rnd()) >> (rnd() & 31);
    case 2: return int32_t(rnd() & 0x3ffff) - 0x20000;
    case 3: {
        static const int32_t edge[] = {0, 1, -1, 65535, 65536, -65536, 32767, 32768, -32768, 0x7fffffff,
                                       int32_t(0x80000000), int32_t(0x80000001), 0x7fff0000, int32_t(0xffff0000)};
        return edge[rnd() % (sizeof edge / sizeof edge[0])];
    }
    default: return int32_t(rnd()) >> 8;
    }
}

int main()
{
    unsigned long long checks = 0, bad = 0;
    for (long i = 0; i < 20000000; i++) {
        const int32_t a = pick(), b = pick();
        // multiply, rounded (operator*) and truncated (the sine's interpolation, the angle to turns)
        for (uint32_t round = 0; round <= 32768u; round += 32768u) {
            const int32_t want = int32_t((int64_t(a) * int64_t(b) + round) >> 16);
            const int32_t got = fixedMulShr16(a, b, round);
            checks++;
            if (got != want && bad++ < 10) std::printf("MUL %ld * %ld (+%lu): %ld, want %ld\n", (long)a, (long)b,
                                                       (unsigned long)round, (long)got, (long)want);
        }
        // divide: (hi:lo) / d with hi < d, exactly as Fixed::operator/ feeds it
        {
            const uint32_t d = uint32_t(b) ? uint32_t(b) : 1u;
            const uint32_t hi = rnd() % d, lo = rnd();
            const uint64_t n = (uint64_t(hi) << 32) | lo;
            const uint32_t want = uint32_t(n / d);
            const uint32_t got = fixedDiv6432(hi, lo, d);
            checks++;
            if (got != want && bad++ < 10)
                std::printf("DIV %lu:%lu / %lu: %lu, want %lu\n", (unsigned long)hi, (unsigned long)lo, (unsigned long)d,
                            (unsigned long)got, (unsigned long)want);
        }
        // and the same through the Fixed operator the game calls
        {
            const Fixed fa = Fixed::fromRaw(a), fb = Fixed::fromRaw(b);
            checks++;
            if ((fa * fb).v != int32_t((int64_t(a) * int64_t(b) + 32768) >> 16) && bad++ < 10)
                std::printf("operator* %ld %ld\n", (long)a, (long)b);
        }
    }
    // the sine and the angle conversion over every table step and then some
    for (int32_t r = -0x7ffff; r <= 0x7ffff; r += 7) {
        const Fixed x = Fixed::fromRaw(r);
        const uint32_t turn = uint32_t((int64_t(r) * 683565276LL) >> 16);
        checks++;
        if (fixedTurn(x) != turn && bad++ < 10) std::printf("TURN %ld\n", (long)r);
    }
    std::printf("test_fixed060: %llu checks, %llu differ\n", checks, bad);
    return bad ? 1 : 0;
}
