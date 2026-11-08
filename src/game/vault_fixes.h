#pragma once

#include <cstdint>
#include <string>

namespace VaultFixes
{
    // Changes car values in the game's attribute database (GLOBAL\ATTRIBUTES.BIN) while the game
    // loads it, the same edits the NFS-VltEd scripts make on disk: no tank slapper, no speed
    // limiter. The file on disk is never changed, and a file already edited with VltEd is fine.
    void Init(uintptr_t baseModule);

    // BLAKE2b-256 of the attribute database with every fix made, in hex, so a server can tell stock
    // game data from edited data. Empty until the game has loaded it.
    const std::string& FilesHash();

    // "unlimited" when every car's speed limiter is off in the data the game loaded (by the client's
    // fix or by VltEd), "limited" when any car has one, "" before the game has loaded it.
    const char* SpeedLimit();
}
