// SPDX-License-Identifier: MIT

// The checks that the unit tests share. A failed check is reported and counted. The test continues. One run then lists
// every failure.

#pragma once

#include <cstdio>
#include <string>

// The number of checks that have failed so far.
inline int failures = 0;

#define WSF_CHECK(cond)                                                                   \
    do                                                                                    \
    {                                                                                     \
        if (!(cond))                                                                      \
        {                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

// Reports and counts a failure if `ok` is false. `what` describes the failure. Use it for a check whose message must
// say more than its condition, such as which case failed.
inline void Check(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::fprintf(stderr, "CHECK failed: %s\n", what.c_str());
        failures++;
    }
}
