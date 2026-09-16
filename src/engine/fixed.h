// 16.16 fixed point for the SF2000 build (CR_FIXED, see real.h): the console has no FPU, and soft-float made one logic
// step cost 3.85 ms and the scene graph update 34 ms (docs/PROGRESS.md), so nothing there may use float or double at
// run time. Range +-32768, step 1/65536.
//
// Construction from a double/float literal is allowed and folds at compile time; a double that survives to run time
// is a bug, and build/check_softfloat.sh fails the SF2000 build when any soft-float routine is called.
// Integers convert exactly. Products and quotients round through int64 (MIPS32 `mult` gives the 64-bit product).
// Values that come from files as IEEE floats go through fixedFromFloatBits(), which only shifts bits.
#pragma once

#include <cstdint>
#include <cstring>

#include "fixed_tables.h"

namespace cr {

struct Fixed {
    int32_t v = 0; // raw value * 65536

    constexpr Fixed() = default;
    constexpr Fixed(int i) : v(int32_t(uint32_t(i) << 16)) {}
    constexpr Fixed(unsigned i) : v(int32_t(i << 16)) {}
    constexpr Fixed(long i) : v(int32_t(uint32_t(i) << 16)) {}
    constexpr Fixed(unsigned long i) : v(int32_t(uint32_t(i) << 16)) {}
    constexpr Fixed(long long i) : v(int32_t(uint32_t(i) << 16)) {}
    constexpr Fixed(unsigned long long i) : v(int32_t(uint32_t(i) << 16)) {}
    // literals only (see above)
    constexpr Fixed(double d) : v(int32_t(d >= 0 ? d * 65536.0 + 0.5 : d * 65536.0 - 0.5)) {}
    constexpr Fixed(float f) : Fixed(double(f)) {}

    static constexpr Fixed fromRaw(int32_t r)
    {
        Fixed f;
        f.v = r;
        return f;
    }

    // C++ int(double) semantics: truncation toward zero
    constexpr explicit operator int() const { return v >= 0 ? (v >> 16) : -((-v) >> 16); }
    constexpr explicit operator bool() const { return v != 0; }
    // for host-side tools and logs only (never in the core)
    constexpr double toDouble() const { return double(v) / 65536.0; }

    constexpr Fixed operator-() const { return fromRaw(-v); }
    constexpr Fixed operator+() const { return *this; }

    friend constexpr Fixed operator+(Fixed a, Fixed b) { return fromRaw(a.v + b.v); }
    friend constexpr Fixed operator-(Fixed a, Fixed b) { return fromRaw(a.v - b.v); }
    friend constexpr Fixed operator*(Fixed a, Fixed b)
    {
        return fromRaw(int32_t((int64_t(a.v) * int64_t(b.v) + 32768) >> 16));
    }
    // division by zero gives 0 (JavaScript would give Infinity/NaN; the game guards those cases anyway)
    friend Fixed operator/(Fixed a, Fixed b)
    {
        if (b.v == 0) return fromRaw(0);
        // C division truncates toward zero, so half the divisor is added away from zero: round half away from zero
        const int64_t n = int64_t(a.v) * 65536;
        const int64_t half = (b.v > 0 ? int64_t(b.v) : -int64_t(b.v)) / 2;
        return fromRaw(int32_t((n >= 0 ? n + half : n - half) / b.v));
    }

    Fixed &operator+=(Fixed o) { v += o.v; return *this; }
    Fixed &operator-=(Fixed o) { v -= o.v; return *this; }
    Fixed &operator*=(Fixed o) { return *this = *this * o; }
    Fixed &operator/=(Fixed o) { return *this = *this / o; }

    friend constexpr bool operator==(Fixed a, Fixed b) { return a.v == b.v; }
    friend constexpr bool operator!=(Fixed a, Fixed b) { return a.v != b.v; }
    friend constexpr bool operator<(Fixed a, Fixed b) { return a.v < b.v; }
    friend constexpr bool operator<=(Fixed a, Fixed b) { return a.v <= b.v; }
    friend constexpr bool operator>(Fixed a, Fixed b) { return a.v > b.v; }
    friend constexpr bool operator>=(Fixed a, Fixed b) { return a.v >= b.v; }
};

constexpr Fixed kFixedMax = Fixed::fromRaw(0x7fffffff);
constexpr Fixed kFixedEpsilon = Fixed::fromRaw(1);
constexpr Fixed kFixedPi = Fixed::fromRaw(205887);    // 3.14159 * 65536
constexpr Fixed kFixedTwoPi = Fixed::fromRaw(411775); // 6.28318 * 65536

inline constexpr Fixed fixedFloor(Fixed a) { return Fixed::fromRaw(int32_t(uint32_t(a.v) & 0xffff0000u)); }
inline constexpr Fixed fixedCeil(Fixed a) { return Fixed::fromRaw(int32_t(uint32_t(a.v + 0xffff) & 0xffff0000u)); }
// JavaScript Math.round: floor(x + 0.5)
inline constexpr Fixed fixedRound(Fixed a) { return Fixed::fromRaw(int32_t(uint32_t(a.v + 0x8000) & 0xffff0000u)); }
inline constexpr Fixed fixedAbs(Fixed a) { return a.v < 0 ? -a : a; }
// a / Fixed(2) without the 64-bit division, bit for bit: that operator rounds half away from zero
inline constexpr Fixed fixedHalf(Fixed a)
{
    return Fixed::fromRaw(a.v >= 0 ? int32_t((int64_t(a.v) + 1) >> 1) : int32_t(-((-int64_t(a.v) + 1) >> 1)));
}

// sine and cosine of an angle in radians: the angle becomes a 32-bit fraction of a full turn (wraps for free), the
// top 10 bits index kSinQ16 and the next 16 bits interpolate
inline Fixed fixedSinTurn(uint32_t turn)
{
    const uint32_t index = turn >> (32 - kSinTableBits);
    const int32_t frac = int32_t((turn >> (32 - kSinTableBits - 16)) & 0xffff);
    const int32_t a = kSinQ16[index], b = kSinQ16[index + 1];
    return Fixed::fromRaw(a + int32_t((int64_t(b - a) * frac) >> 16));
}
inline uint32_t fixedTurn(Fixed radians)
{
    // 2^32 / (2 pi) in 16.16 = 683565275.6
    return uint32_t((int64_t(radians.v) * 683565276LL) >> 16);
}
inline Fixed fixedSin(Fixed radians) { return fixedSinTurn(fixedTurn(radians)); }
inline Fixed fixedCos(Fixed radians) { return fixedSinTurn(fixedTurn(radians) + 0x40000000u); }

// Math.atan2(Math.sin(a), Math.cos(a)): the angle wrapped into (-pi, pi]
inline Fixed fixedNormalizeAngle(Fixed a)
{
    int32_t r = a.v % kFixedTwoPi.v;
    if (r > kFixedPi.v) r -= kFixedTwoPi.v;
    else if (r <= -kFixedPi.v) r += kFixedTwoPi.v;
    return Fixed::fromRaw(r);
}

inline Fixed fixedSqrt(Fixed a)
{
    if (a.v <= 0) return Fixed::fromRaw(0);
    // raw result = sqrt(raw << 16), integer square root bit by bit
    uint64_t n = uint64_t(uint32_t(a.v)) << 16;
    uint64_t root = 0, bit = uint64_t(1) << 46;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    if (n > root) root++; // round to nearest
    return Fixed::fromRaw(int32_t(root));
}

// IEEE-754 single (as stored in the .mesh files) to 16.16 with shifts only; saturates, NaN/Inf/denormal -> 0
inline Fixed fixedFromFloatBits(uint32_t bits)
{
    const int exponent = int((bits >> 23) & 0xff);
    if (exponent == 0 || exponent == 0xff) return Fixed::fromRaw(0);
    const int64_t mantissa = int64_t((bits & 0xffffff) | 0x800000); // value = mantissa * 2^(exponent - 150)
    const int shift = exponent - 150 + 16;
    int64_t raw;
    if (shift >= 0) raw = shift > 31 ? int64_t(0x7fffffff) : (mantissa << shift);
    else if (shift < -40) raw = 0;
    else raw = (mantissa + (int64_t(1) << (-shift - 1))) >> -shift;
    if (raw > 0x7fffffff) raw = 0x7fffffff;
    return Fixed::fromRaw(int32_t((bits & 0x80000000u) ? -raw : raw));
}

inline Fixed fixedFromFloat(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits); // a copy of the bits, not float arithmetic
    return fixedFromFloatBits(bits);
}

} // namespace cr
