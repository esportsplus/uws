#ifndef UWS_BLOOMFILTER_H
#define UWS_BLOOMFILTER_H

/* This filter has no false positives or collisions for the standard
 * and non-standard common request headers */

#include <cstdint>
#include <string_view>
#include <bitset>

namespace uWS {

struct BloomFilter {
private:
    std::bitset<256> filter;
    static inline uint32_t perfectHash(uint32_t features) {
        return features * 1843993368;
    }

    union ScrambleArea {
        unsigned char p[4];
        uint32_t val;
    };

    ScrambleArea getFeatures(std::string_view key) {
        ScrambleArea s;
        s.p[0] = reinterpret_cast<const unsigned char&>(key[0]);
        s.p[1] = reinterpret_cast<const unsigned char&>(key[key.length() - 1]);
        s.p[2] = reinterpret_cast<const unsigned char&>(key[key.length() - 2]);
        s.p[3] = reinterpret_cast<const unsigned char&>(key[key.length() >> 1]);
        return s;
    }

public:
    bool mightHave(std::string_view key) {
        if (key.length() < 2) {
            return true;
        }
    
        ScrambleArea s = getFeatures(key);
        s.val = perfectHash(s.val);
        return filter[s.p[0]] &&
        filter[s.p[1]] &&
        filter[s.p[2]] &&
        filter[s.p[3]];
    }

    void add(std::string_view key) {
        if (key.length() >= 2) {
            ScrambleArea s = getFeatures(key);
            s.val = perfectHash(s.val);
            filter[s.p[0]] = 1;
            filter[s.p[1]] = 1;
            filter[s.p[2]] = 1;
            filter[s.p[3]] = 1;
        }
    }

    void reset() {
        filter.reset();
    }
};

}

#endif // UWS_BLOOMFILTER_H
