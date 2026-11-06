#pragma once

#include <cstdint>

namespace VaultFixes
{
    // Changes car values in the game's attribute database (GLOBAL\ATTRIBUTES.BIN) while the game
    // loads it, the same edits the NFS-VltEd scripts make on disk: no tank slapper, no speed
    // limiter. The file on disk is never changed, and a file already edited with VltEd is fine.
    void Init(uintptr_t baseModule);
}
