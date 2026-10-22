// Online mode: our lobby (notes/map/lobby_protocol.md) stands in for PSN. Off unless mobile_run gets a lobby URL
// (--lobby / $DCP_LOBBY_URL); then NetCtl reports a connection, NpManager a signed-in player, the net-check dialog
// succeeds and SceNpMatching2 / SceLibRudp run on the lobby client and the UDP carrier (src/net). Offline (the
// default) every one of those libraries answers as before: never connected, never signed in.
#pragma once
#include <cstdint>
#include <string>

namespace hle {

// Called once by mobile_run before guest code runs. An empty URL keeps the game offline.
void online_configure(const std::string& lobby_url, const std::string& name);
bool online_enabled();
const std::string& online_lobby_url();

// Our player, the same identity as the Pro port (port/src/hle/np.cpp): online id (PSN charset, <= 16 chars:
// --online-name, else the login name, net::online_name) and the account id + secret of identity.ini in the user
// folder (net/identity.h; verified hello). The lobby only ever sees account ids + names, so a Vita player and a Pro
// player are the same kind of lobby member. Account from the name (unverified) when identity.ini can't be used.
struct NpIdentity {
  std::string online_id;
  uint64_t account_id = 0;
  std::string secret;  // "" = unverified hello
};
const NpIdentity& np_identity();

// SceNpId (0x24 bytes, notes/map/vita_net_109.md 4.7) = only the game-facing wrapper of a lobby member, derived the
// same way on every peer: online id at +0 (handle, <= 16 chars + NUL), the 64-bit account id little endian in the opt
// bytes at +0x14, reserved (+0x1c) zero. np_id_account() reads the account back (0 = not one of ours).
constexpr uint32_t NP_ID_SIZE = 0x24;
constexpr uint32_t NP_ID_ACCOUNT = 0x14;
void make_np_id(uint8_t* out, const std::string& online_id, uint64_t account_id);
uint64_t np_id_account(const uint8_t* np_id);

}  // namespace hle
