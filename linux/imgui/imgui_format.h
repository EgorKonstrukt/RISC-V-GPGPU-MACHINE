// Text from numbers for Dear ImGui (docs/apps/imgui.md): what the C library's vsnprintf would
// write, for the formats an interface is full of, without its two passes and its long doubles.
// -1 for anything else, which the library then does. Included by imgui_impl_shaderemu.cpp.
#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

// A double to `precision` places, rounded as the library rounds (the exact value, ties to
// even): its digits before the point and after, as two whole numbers. False when the sums
// do not fit 64 bits, which a float under four thousand million to nine places always does.
static bool format_fixed(double value, int precision, uint32_t* whole, uint32_t* part, bool* negative)
{
    static const uint32_t tens[10] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
    uint64_t b;
    memcpy(&b, &value, 8);
    int e = (int)(b >> 52 & 0x7ff);
    uint64_t m = b & 0xfffffffffffffull, scaled = 0;
    *negative = b >> 63 != 0;
    if (e == 0x7ff || precision > 9 || (e == 0 && m)) return false;
    if (e) {
        // the value is m * 2^e; one that was a float has 29 bits of nothing at its end
        m |= 1ull << 52, e -= 1075;
        if (!((uint32_t)m & 0x1fffffff)) m >>= 29, e += 29;
        if ((m >> 24 && precision > 3) || e > 0 || e < -63) return false;
        m *= tens[precision];
        scaled = m;
        if (e) {
            uint64_t half = 1ull << (-e - 1), rest = m & (half << 1) - 1;
            scaled = m >> -e;
            if (rest > half || (rest == half && (scaled & 1))) scaled++;
        }
        if (scaled >> 32) return false;
    }
    *whole = (uint32_t)scaled / tens[precision];
    *part = (uint32_t)scaled % tens[precision];
    return true;
}

// The length the text has in full (as vsnprintf says it), with as much of it as fits in `buf`
// and no 0 after it; or -1 with `buf` in any state. `size` is at least 1.
static int format_quick(char* buf, size_t size, const char* fmt, va_list args)
{
    char *to = buf, *end = buf + size - 1, digits[24];
    int length = 0;
    for (;;) {
        char c = *fmt++;
        if (c != '%') {
            if (!c) return length;
            if (to < end) *to++ = c;
            length++;
            continue;
        }
        bool left = false, zero = false, plus = false, blank = false, negative = false;
        for (;; fmt++) {
            c = *fmt;
            if (c == '-') left = true;
            else if (c == '0') zero = true;
            else if (c == '+') plus = true;
            else if (c == ' ') blank = true;
            else break;
        }
        int width = 0, precision = -1, count = 0, zeros = 0;
        while (*fmt >= '0' && *fmt <= '9' && width < 1000) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.')
            for (fmt++, precision = 0; *fmt >= '0' && *fmt <= '9' && precision < 1000;) precision = precision * 10 + (*fmt++ - '0');
        const char* text = digits + sizeof digits;      // the digits are written backwards from the end
        bool number = true;
        c = *fmt++;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X') {
            if (precision >= 0 || (c != 'd' && c != 'i' && (plus || blank))) return -1;
            uint32_t v = va_arg(args, uint32_t);
            char* at = digits + sizeof digits;
            if ((c == 'd' || c == 'i') && (int32_t)v < 0) negative = true, v = 0u - v;
            if (c == 'x' || c == 'X') {
                const char* names = c == 'x' ? "0123456789abcdef" : "0123456789ABCDEF";
                do *--at = names[v & 15]; while (v >>= 4);
            } else
                do *--at = (char)('0' + v % 10); while (v /= 10);
            text = at, count = (int)(digits + sizeof digits - at);
        } else if (c == 'f') {
            uint32_t whole, part;
            if (precision < 0) precision = 6;
            if (!format_fixed(va_arg(args, double), precision, &whole, &part, &negative)) return -1;
            char* at = digits + sizeof digits;
            for (int i = 0; i < precision; i++, part /= 10) *--at = (char)('0' + part % 10);
            if (precision) *--at = '.';
            do *--at = (char)('0' + whole % 10); while (whole /= 10);
            text = at, count = (int)(digits + sizeof digits - at);
        } else if (c == 's') {
            if (zero || plus || blank) return -1;
            text = va_arg(args, const char*);
            if (!text) return -1;
            count = (int)(precision < 0 ? strlen(text) : strnlen(text, (size_t)precision));
            number = false;
        } else if (c == 'c') {
            if (zero || plus || blank || precision >= 0) return -1;
            digits[0] = (char)va_arg(args, int);
            text = digits, count = 1;
            number = false;
        } else if (c == '%') {
            if (fmt[-2] != '%') return -1;
            if (to < end) *to++ = '%';
            length++;
            continue;
        } else
            return -1;
        // the sign, then zeros or (before the sign, or after everything) spaces up to the width
        char sign = negative ? '-' : !number ? 0 : plus ? '+' : blank ? ' ' : 0;
        int spaces = width - count - (sign != 0);
        if (spaces < 0) spaces = 0;
        if (zero && !left) zeros = spaces, spaces = 0;
        length += spaces + zeros + count + (sign != 0);
        if (!left)
            for (; spaces > 0; spaces--)
                if (to < end) *to++ = ' ';
        if (sign && to < end) *to++ = sign;
        for (; zeros > 0; zeros--)
            if (to < end) *to++ = '0';
        for (; count > 0; count--, text++)
            if (to < end) *to++ = *text;
        for (; spaces > 0; spaces--)
            if (to < end) *to++ = ' ';
    }
}
