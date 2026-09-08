/* This module implements URI query parsing and retrieval of value given key */

#ifndef UWS_QUERYPARSER_H
#define UWS_QUERYPARSER_H

#include <string>
#include <string_view>

namespace uWS {

    /* Takes raw query including initial '?' sign and decodes into scratch */
    static inline std::string_view getDecodedQueryValue(std::string_view key, std::string_view rawQuery, std::string &scratch) {

        /* Can't have a value without a key */
        if (!key.length()) {
            return {};
        }

        /* Start with the whole querystring including initial '?' */
        std::string_view queryString = rawQuery;

        /* List of key, value could be cached for repeated fetches similar to how headers are, todo! */
        while (queryString.length()) {
            /* Find boundaries of this statement */
            std::string_view statement = queryString.substr(1, queryString.find('&', 1) - 1);

            /* Only bother if first char of key match (early exit) */
            if (statement.length() && statement[0] == key[0]) {
                /* Equal sign must be present and not in the end of statement */
                auto equality = statement.find('=');
                if (equality != std::string_view::npos) {

                    std::string_view statementKey = statement.substr(0, equality);
                    std::string_view statementValue = statement.substr(equality + 1);

                    /* String comparison */
                    if (key == statementKey) {

                        /* Decode into per-request scratch storage without mutating the raw query */
                        scratch.assign(statementValue.data(), statementValue.length());
                        char *in = scratch.data();

                        /* Write offset */
                        unsigned int out = 0;

                        /* Walk over all chars until end or null char, decoding in place */
                        for (unsigned int i = 0; i < statementValue.length() && in[i]; i++) {
                                /* Only bother with '%' */
                                if (in[i] == '%') {
                                    /* Do we have enough data for two bytes hex? */
                                    if (i + 2 >= statementValue.length()) {
                                        return {};
                                    }

                                    /* Two bytes hex */
                                    auto decodeHex = [](char c) -> int {
                                        if (c >= '0' && c <= '9') {
                                            return c - '0';
                                        }
                                        if (c >= 'A' && c <= 'F') {
                                            return c - 'A' + 10;
                                        }
                                        if (c >= 'a' && c <= 'f') {
                                            return c - 'a' + 10;
                                        }
                                        return -1;
                                    };
                                    int hex1 = decodeHex(in[i + 1]);
                                    int hex2 = decodeHex(in[i + 2]);
                                    if (hex1 < 0 || hex2 < 0) {
                                        return {};
                                    }

                                    *((unsigned char *) &in[out]) = (unsigned char) (hex1 * 16 + hex2);
                                    i += 2;
                                } else {
                                    /* Is this even a rule? */
                                    if (in[i] == '+') {
                                        in[out] = ' ';
                                    } else {
                                        in[out] = in[i];
                                    }
                                }

                                /* We always only write one char */
                                out++;
                        }

                        return std::string_view(scratch.data(), out);
                    }
                }
            }

            queryString.remove_prefix(statement.length() + 1);
        }

        /* Nothing found is given as nullptr, while empty string is given as some pointer to the given buffer */
        return {nullptr, 0};
    }

}

#endif
