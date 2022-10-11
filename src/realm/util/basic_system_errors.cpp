/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <cstdlib>
#include <cstring>
#include <string>

#include <realm/util/features.h>
#include <realm/util/basic_system_errors.hpp>

using namespace realm::util;


namespace {

class system_category : public std::error_category {
    const char* name() const noexcept override;
    std::string message(int) const override;
};

system_category g_system_category;

const char* system_category::name() const noexcept
{
    return "realm.basic_system";
}

std::string system_category::message(int value) const
{
    const size_t max_msg_size = 256;
    char buffer[max_msg_size + 1];

#ifdef _WIN32 // Windows version

    if (REALM_LIKELY(strerror_s(buffer, max_msg_size, value) == 0)) {
        return buffer; // Guaranteed to be NULL-terminated
    }

#elif REALM_PLATFORM_APPLE

    {
        const int result = strerror_r(value, buffer, max_msg_size);
        if (REALM_LIKELY(result == 0 || result == ERANGE || result == EINVAL)) {
            // On Apple platforms, strings generated by ERANGE cases are
            // guaranteed to be NULL-terminated, however, there is no formal
            // indication that this behaviour can be expected in EINVAL cases.
            // Even though the chances are slim, better safe than sorry.
            buffer[max_msg_size] = '\0';
            return buffer;
        }
    }

#elif !REALM_ANDROID && _GNU_SOURCE // GNU specific version

    {
        char* msg = nullptr;
        if (REALM_LIKELY((msg = strerror_r(value, buffer, max_msg_size)) != nullptr)) {
            return msg; // Guaranteed to be NULL-terminated
        }
    }

#else // POSIX.1-2001 fallback version

    {
        const int result = strerror_r(value, buffer, max_msg_size);
        if (REALM_LIKELY(result == 0 || result == EINVAL)) {
            // POSIX doesn't make any guarantees that the string will be
            // NULL-terminated. Better safe than sorry.
            buffer[max_msg_size] = '\0';
            return buffer;
        }
    }

#endif

    return "Unknown error";
}

} // anonymous namespace

const std::error_category& realm::util::basic_system_error_category() noexcept
{
    return g_system_category;
}

namespace realm::util::error {
std::error_code make_error_code(basic_system_errors err) noexcept
{
    return std::error_code(err, g_system_category);
}
} // namespace realm::util::error
