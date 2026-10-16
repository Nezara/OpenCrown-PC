// Online identity (lobby_protocol.md 4.1): a random 64-bit account id + a 32-byte secret in <user dir>/identity.ini,
// shared by the launcher (friends UI) and dcp_run (np_identity -> hello). Created on first use from the Windows CSPRNG
// and never overwritten; the secret proves the account to the lobby (it stores only its SHA-256). Deleting the file
// means a new account (friends are lost).
#pragma once
#include <cstdint>
#include <string>

namespace dcp::net {

struct Identity {
    bool ok = false;
    uint64_t account_id = 0;  // odd, non-zero
    std::string secret;       // 64 lower-case hex chars
    std::string path;         // the identity.ini used
    std::string error;        // why ok is false
};

// The folder holding identity.ini as dcp_run sees it: %DCP_USER_DIR%, else the parent of %DCP_SAVE_ROOT% (the rule
// dcp_run uses for input.ini), else %USERPROFILE%/Documents/Dragons Crown Pro. The launcher passes its own user dir
// as DCP_USER_DIR, so both always use the same file.
std::string identity_dir();

// Reads <dir>/identity.ini, creating it (and dir) when missing. A present but unreadable / malformed file is left
// alone and reported (ok = false): never overwritten.
Identity load_identity(const std::string& dir);

// Online id as the game reports it (np_identity): `configured` (launcher name / $DCP_NP_NAME), else %USERNAME%, kept
// to the PSN charset [A-Za-z0-9_-], <= 16 chars; "Player" when fewer than 3 remain. The launcher's friends sessions use
// it too, so the name friends see is the same whichever of the two said hello last.
std::string online_name(const std::string& configured);

// SHA-256 of a byte string, lower-case hex (BCrypt; used by the mock lobby for the stored secret hash).
std::string sha256_hex(const std::string& data);

// Friend codes (lobby_protocol.md 4.2): 8 chars of the join-code alphabet, shown as XXXX-XXXX. normalize accepts
// either form, any case, spaces / dashes anywhere; returns "" when it is not a valid code.
std::string friend_code_normalize(const std::string& s);
std::string friend_code_display(const std::string& code8);

}  // namespace dcp::net
