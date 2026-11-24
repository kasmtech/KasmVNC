/* Copyright (C) 2026 Kasm Technologies Corp.  All Rights Reserved.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include <rfb/webrtc/TurnConfig.h>

namespace rfb::turn {

    struct Snapshot {
        Credentials credentials;
        bool forceRelay = false;
        uint64_t generation = 0;
    };

    std::optional<std::string> readSecretFile(const std::string &path, std::string &error);

    std::optional<Credentials> fetchCredentials(
        const std::string &url, const std::string &caFile, bool skipVerify, std::string &failureReason);

    class CredentialSource {
    public:
        // Never destroyed: the detached refresh thread may still be running at process exit.
        static CredentialSource &instance();

        void start();
        Snapshot snapshot();

    private:
        CredentialSource() = default;
        void refreshLoop();
        bool replaceCredentials(Credentials creds);

        std::mutex mutex;
        bool started = false;
        Snapshot current;
        std::string url, caFile;
        bool skipVerify = false;
    };

} // namespace rfb::turn
