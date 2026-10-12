// What the dashboard tab shows about PCs on the link (ticket 06 provides it, ticket 05 draws it).
// Kept apart from link_server.h so the settings panel doesn't pull in sockets.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/** The link's pairing state, for the Allow / Deny banner and the "Forget PCs" button. */
struct LinkUiState {
    struct Pending {
        std::string id;    ///< the PC's 128-bit id as 32 lowercase hex chars (pass it back to allow / deny)
        std::string name;  ///< the PC's computer name, as it sent it (UTF-8, ≤ 63 bytes)
    };
    std::vector<Pending> pending;  ///< PCs waiting for Allow / Deny, oldest first
    size_t knownPcs = 0;           ///< entries in config allowed_pcs ("Forget PCs (N)")
    uint64_t revision = 0;         ///< bumps whenever anything above changes (redraw when it moves)
};
