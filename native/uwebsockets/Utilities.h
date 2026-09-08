#ifndef UWS_UTILITIES_H
#define UWS_UTILITIES_H

/* Various common utilities */

#include <cstdint>

namespace uWS {
namespace utils {

inline int u32toaHex(uint32_t value, char *dst) {
    char palette[] = "0123456789abcdef";
    char temp[10];
    char *p = temp;
    do {
        *p++ = palette[value & 15];
        value >>= 4;
    } while (value > 0);

    int ret = (int) (p - temp);

    do {
        *dst++ = *--p;
    } while (p != temp);

    return ret;
}

inline int u64toa(uint64_t value, char *dst) {
    char temp[20];
    char *p = temp;
    do {
        *p++ = (char) ((value % 10) + '0');
        value /= 10;
    } while (value > 0);

    int ret = (int) (p - temp);

    do {
        *dst++ = *--p;
    } while (p != temp);

    return ret;
}

}
}

#endif // UWS_UTILITIES_H
