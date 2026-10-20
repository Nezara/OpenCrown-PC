// Online session trace (diagnostics only, $DCP_NET_TRACE=1; off = game code untouched). Logs, with the frame clock,
// the eboot's session decisions that notes/map/town_session.md section 9 asks for: every call of the leave /
// dissolve family (9d9e0, 9bb70, baba0, 9c6c0, host-bit 9c5b0 / 9c5d0, close bad20, kick 9b8f0 / a2ce0, slot remap
// a06c0) with the return address and the Act mode / sub-state; Act::ChangeMode (43c6b0), Act::OnNetError (43f5b0),
// EnterAcceptConnect (43f570 / a0200); the connect-check tasks 9f550 / 9f1e0 / 9d8a0 and GameServer::ConnectTask 28330
// (state changes only); NetCtrl::ReceiveCommand (9e980) accept / drop with the 0x14-B command header and both scene
// tags; command sends (9d5e0); the game's own debug text helpers (GameServer 28770, Act 4364d0); and a 2 ms watcher of
// Act / platform-sys / DcNet / matching state fields. Hooks are int3 + a relay holding the displaced instruction (no
// re-arm window), every site checked against its expected bytes first. Lines start with "NETTRACE".
#pragma once
#include <cstdint>

namespace dcp::net_trace {

void install(uint8_t* eboot_base);

}  // namespace dcp::net_trace
