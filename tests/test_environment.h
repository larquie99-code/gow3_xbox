// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cassert>
#include <cstdlib>

inline void SetTestEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    const int result = _putenv_s(name, value);
#else
    const int result = *value ? setenv(name, value, 1) : unsetenv(name);
#endif
    assert(result == 0);
}
