// SceCommonDialog: message, save-data, network-check, IME and trophy-setup dialogs.
//
// No dialog UI yet: each dialog answers the way a player normally would and logs what it showed. Dialogs with
// buttons report OK / YES (or the first of three buttons) on the second status poll; dialogs without buttons
// (progress, "please wait") stay running until the game finishes or terminates them. The save-data list picks the
// focused / first slot (for loading, the first slot that exists); the network check reports "cancelled" offline and
// success in online mode (hle/online.h);
// the IME dialog waits for the host's text-entry UI (hle/ime.h; platform/text_entry) and, headless, returns its
// initial text. A real overlay for the other dialogs replaces these answers later; the status / result plumbing stays.
//
// Layouts: vitasdk psp2/message_dialog.h, ime_dialog.h, common_dialog.h; save-data / net-check / trophy-setup
// dialogs from Vita3K's dialog types (reference only). SceCommonDialogParam is 0x4C bytes.
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

#include "hle/common.h"
#include "hle/ime.h"
#include "hle/online.h"

namespace hle {

bool apputil_slot_exists(Cpu& c, uint32_t id, uint32_t out);

namespace {

enum : uint32_t {
  STATUS_NONE = 0,
  STATUS_RUNNING = 1,
  STATUS_FINISHED = 2,
  RESULT_OK = 0,
  RESULT_USER_CANCELED = 1,
  ERROR_BUSY = 0x80020401,
  ERROR_NULL = 0x80020402,
  ERROR_NOT_RUNNING = 0x80020404,
  ERROR_NOT_FINISHED = 0x80020410,
  ERROR_NOT_IN_USE = 0x80020411,
};

enum Kind { NONE, MSG, SAVEDATA, NETCHECK, IME, TROPHY };

struct ImeState {
  uint64_t id = 0;
  uint32_t max = 0, type = 0, option = 0, buffer = 0;
  bool cancelable = true, written = false;
  std::u16string title, initial, text;
};

struct Dialog {
  Kind kind = NONE;
  uint32_t status = STATUS_NONE;
  bool waits = false;  // no buttons: runs until finished / terminated by the game
  int polls = 0;
  int32_t mode = 0, result = RESULT_OK, button = 1, slot = 0;
  uint32_t userdata = 0;
  ImeState ime;
};
std::mutex dialog_lock;
Dialog active;

uint32_t begin(Kind k) {
  if (active.kind != NONE && active.status != STATUS_NONE) return ERROR_BUSY;
  active = Dialog{};
  active.kind = k;
  active.status = STATUS_RUNNING;
  return 0;
}

uint32_t status(Kind k) {
  if (active.kind != k) return STATUS_NONE;
  if (active.status == STATUS_RUNNING && !active.waits && ++active.polls >= 2) active.status = STATUS_FINISHED;
  return active.status;
}

uint32_t term(Kind k) {
  if (active.kind != k || active.status == STATUS_NONE) return ERROR_NOT_IN_USE;
  if (active.status == STATUS_RUNNING && !active.waits) return ERROR_NOT_FINISHED;
  active = Dialog{};
  return 0;
}

std::string text(Cpu& c, uint32_t va) { return va ? str(c, va, 512) : std::string("(none)"); }

// --- SceCommonDialog ------------------------------------------------------------------------------------------
void Update(Cpu& c) { ret(c, 0); }  // the dialog overlay would be drawn here into the game's buffer

// --- message dialog --------------------------------------------------------------------------------------------
// SceMsgDialogParam: mode 0x50, userMsg* 0x54 {buttonType, msg*}, sysMsg* 0x58 {type, value},
// errorCode* 0x5C {code}, progressBar* 0x60 {barType, sysMsg{type,value}, msg*}.
void MsgDialogInit(Cpu& c) {
  uint32_t p = arg(c, 0);
  if (!p) return ret(c, ERROR_NULL);
  std::lock_guard lk(dialog_lock);
  if (uint32_t e = begin(MSG)) return ret(c, e);
  Memory& m = c.mem();
  active.mode = int32_t(m.read<uint32_t>(p + 0x50));
  switch (active.mode) {
    case 1: {  // user message
      uint32_t u = m.read<uint32_t>(p + 0x54);
      uint32_t type = u ? m.read<uint32_t>(u) : 0;
      HLE_NOTE("[dialog] message (buttons %u): %s\n", type, u ? text(c, m.read<uint32_t>(u + 4)).c_str() : "");
      active.waits = type == 2;  // SCE_MSG_DIALOG_BUTTON_TYPE_NONE
      if (type == 4) active.result = RESULT_USER_CANCELED, active.button = 0;  // CANCEL only
      break;
    }
    case 2: {  // system message
      uint32_t s = m.read<uint32_t>(p + 0x58);
      uint32_t type = s ? m.read<uint32_t>(s) : 0;
      HLE_NOTE("[dialog] system message type %u\n", type);
      active.waits = type == 1 || type == 5 || type == 6;  // the "please wait" kinds
      break;
    }
    case 3: {
      uint32_t e = m.read<uint32_t>(p + 0x5C);
      HLE_NOTE("[dialog] error code 0x%08x\n", e ? m.read<uint32_t>(e) : 0);
      break;
    }
    case 4: {
      uint32_t b = m.read<uint32_t>(p + 0x60);
      HLE_NOTE("[dialog] progress bar: %s\n", b ? text(c, m.read<uint32_t>(b + 12)).c_str() : "");
      active.waits = true;
      break;
    }
    default: active = Dialog{}; return ret(c, 0x80020403);  // SCE_COMMON_DIALOG_ERROR_INVALID_ARGUMENT
  }
  ret(c, 0);
}
void MsgDialogGetStatus(Cpu& c) {
  std::lock_guard lk(dialog_lock);
  ret(c, status(MSG));
}
// SceMsgDialogResult {mode, result, buttonId}
void MsgDialogGetResult(Cpu& c) {
  uint32_t r = arg(c, 0);
  if (!r) return ret(c, ERROR_NULL);
  std::lock_guard lk(dialog_lock);
  if (active.kind != MSG || active.status != STATUS_FINISHED) return ret(c, ERROR_NOT_FINISHED);
  std::memset(c.mem().ptr(r), 0, 0x2C);
  c.mem().write<int32_t>(r, active.mode);
  c.mem().write<int32_t>(r + 4, active.result);
  c.mem().write<int32_t>(r + 8, active.button);
  ret(c, 0);
}
void MsgDialogTerm(Cpu& c) {
  std::lock_guard lk(dialog_lock);
  if (active.kind == MSG) active.waits = false, active.status = STATUS_FINISHED;
  ret(c, term(MSG));
}

// --- save-data dialog ----------------------------------------------------------------------------------------------
// SceSaveDataDialogParam: mode 0x50, dispType 0x54 (1 save, 2 load, 3 delete), fixed* 0x58 {slot{id,...}},
// list* 0x5C {slots*, count, focusPos, focusId}, userMsg* 0x60 {buttonType, msg*}, sysMsg* 0x64 {type, value},
// errorCode* 0x68 {code}, progressBar* 0x6C, userdata 0x78. Slots are 16 bytes {id, status, userParam, empty*}.
void savedata_apply(Cpu& c, uint32_t p) {
  Memory& m = c.mem();
  active.mode = int32_t(m.read<uint32_t>(p + 0x50));
  uint32_t disp = m.read<uint32_t>(p + 0x54);
  active.userdata = m.read<uint32_t>(p + 0x78);
  active.result = RESULT_OK;
  active.button = 1;
  active.waits = false;
  active.polls = 0;
  active.status = STATUS_RUNNING;
  switch (active.mode) {
    case 1: {  // fixed slot
      uint32_t f = m.read<uint32_t>(p + 0x58);
      active.slot = f ? int32_t(m.read<uint32_t>(f)) : 0;
      HLE_NOTE("[dialog] save data (type %u): fixed slot %d\n", disp, active.slot);
      break;
    }
    case 2: {  // list: focused / first slot; loading prefers a slot that exists
      uint32_t l = m.read<uint32_t>(p + 0x5C);
      uint32_t slots = l ? m.read<uint32_t>(l) : 0, n = l ? m.read<uint32_t>(l + 4) : 0;
      uint32_t focus = l ? m.read<uint32_t>(l + 8) : 0;
      active.slot = n ? int32_t(m.read<uint32_t>(slots)) : 0;
      if (focus == 8) active.slot = int32_t(m.read<uint32_t>(l + 12));  // FOCUS_POS_ID
      if (disp == 2) {
        bool found = false;
        for (uint32_t i = 0; i < n && !found; i++) {
          uint32_t id = m.read<uint32_t>(slots + 16 * i);
          if (apputil_slot_exists(c, id, 0)) active.slot = int32_t(id), found = true;
        }
        if (!found) active.result = RESULT_USER_CANCELED, active.button = 0;
      }
      HLE_NOTE("[dialog] save data (type %u): list of %u -> slot %d%s\n", disp, n, active.slot,
               active.result ? " (nothing to load: cancelled)" : "");
      break;
    }
    case 3: {  // user message
      uint32_t u = m.read<uint32_t>(p + 0x60);
      uint32_t type = u ? m.read<uint32_t>(u) : 0;
      HLE_NOTE("[dialog] save data message (buttons %u): %s\n", type, u ? text(c, m.read<uint32_t>(u + 4)).c_str() : "");
      active.waits = type == 2;
      break;
    }
    case 4: {  // system message: progress ("saving...") waits, the rest are answered YES / OK
      uint32_t s = m.read<uint32_t>(p + 0x64);
      uint32_t type = s ? m.read<uint32_t>(s) : 0;
      HLE_NOTE("[dialog] save data system message type %u\n", type);
      active.waits = type == 5;
      break;
    }
    case 5: {
      uint32_t e = m.read<uint32_t>(p + 0x68);
      HLE_NOTE("[dialog] save data error 0x%08x\n", e ? m.read<uint32_t>(e) : 0);
      break;
    }
    case 6:
      HLE_NOTE("[dialog] save data progress bar\n");
      active.waits = true;
      break;
  }
}
void SaveDataDialogInit(Cpu& c) {
  if (!arg(c, 0)) return ret(c, ERROR_NULL);
  std::lock_guard lk(dialog_lock);
  if (uint32_t e = begin(SAVEDATA)) return ret(c, e);
  savedata_apply(c, arg(c, 0));
  ret(c, 0);
}
// sceSaveDataDialogContinue(param*): the running dialog switches to a new page.
void SaveDataDialogContinue(Cpu& c) {
  if (!arg(c, 0)) return ret(c, ERROR_NULL);
  std::lock_guard lk(dialog_lock);
  if (active.kind != SAVEDATA || active.status == STATUS_NONE) return ret(c, ERROR_NOT_RUNNING);
  savedata_apply(c, arg(c, 0));
  ret(c, 0);
}
// sceSaveDataDialogFinish(finishParam*): closes a waiting page.
void SaveDataDialogFinish(Cpu& c) {
  std::lock_guard lk(dialog_lock);
  if (active.kind != SAVEDATA || active.status != STATUS_RUNNING) return ret(c, ERROR_NOT_RUNNING);
  active.waits = false;
  active.status = STATUS_FINISHED;
  ret(c, 0);
}
void SaveDataDialogGetStatus(Cpu& c) {
  std::lock_guard lk(dialog_lock);
  ret(c, status(SAVEDATA));
}
void SaveDataDialogGetSubStatus(Cpu& c) { ret(c, 0); }  // SCE_SAVEDATA_DIALOG_SUBSTATUS_NONE
void SaveDataDialogTerm(Cpu& c) {
  std::lock_guard lk(dialog_lock);
  if (active.kind == SAVEDATA) active.waits = false, active.status = STATUS_FINISHED;
  ret(c, term(SAVEDATA));
}

// --- network check dialog: cancelled offline, passed online (our lobby stands in for PSN) ----------------------
// The game opens it with mode 3 (PSN online, f813b8246) and only waits for FINISHED; the result is not read (f813b8352).
void NetCheckDialogInit(Cpu& c) {
  if (!arg(c, 0)) return ret(c, ERROR_NULL);
  std::lock_guard lk(dialog_lock);
  if (uint32_t e = begin(NETCHECK)) return ret(c, e);
  bool online = online_enabled();
  HLE_NOTE("[dialog] network check (mode %u): %s\n", c.mem().read<uint32_t>(arg(c, 0) + 0x50),
           online ? "online, passed" : "offline, cancelled");
  active.result = online ? RESULT_OK : RESULT_USER_CANCELED;
  active.slot = online;  // psnModeSucceeded
  ret(c, 0);
}
void NetCheckDialogGetStatus(Cpu& c) {
  std::lock_guard lk(dialog_lock);
  ret(c, status(NETCHECK));
}
// SceNetCheckDialogResult {result, psnModeSucceeded, reserved[124]}
void NetCheckDialogGetResult(Cpu& c) {
  uint32_t r = arg(c, 0);
  if (!r) return ret(c, ERROR_NULL);
  std::lock_guard lk(dialog_lock);
  if (active.kind != NETCHECK || active.status != STATUS_FINISHED) return ret(c, ERROR_NOT_FINISHED);
  std::memset(c.mem().ptr(r), 0, 0x84);
  c.mem().write<int32_t>(r, active.result);
  c.mem().write<int32_t>(r + 4, active.slot);  // psnModeSucceeded
  ret(c, 0);
}
void NetCheckDialogTerm(Cpu& c) {
  std::lock_guard lk(dialog_lock);
  ret(c, term(NETCHECK));
}

// --- IME dialog -----------------------------------------------------------------------------------------------------
// SceImeDialogParam (0xA8 bytes, asm f810749a8 in 1.09): sdkVersion 0x00, inputMethod 0x04, supportedLanguages u64 0x08,
// languagesForced 0x10, type 0x14, option 0x18, filter* 0x1C, dialogMode 0x20 (1 = WITH_CANCEL), textBoxMode 0x24,
// title* 0x28, maxTextLength 0x2C, initialText* 0x30, inputTextBuffer* 0x34 (all UTF-16), commonParam 0x38..0x84
// (magic at 0x80), enterLabel 0x84. The game reads only SceImeDialogResult.result (0 OK, 1 cancelled, 2 aborted) and
// then copies inputTextBuffer whatever the result (notes/map/vita_ime_109.md).
// With a host UI (ime.h) the dialog runs until the host answers; headless it enters the initial text at once.
enum : uint32_t { IME_BUTTON_NONE = 0, IME_BUTTON_CLOSE = 1, IME_BUTTON_ENTER = 2, RESULT_ABORTED = 2 };
bool ime_host = false;
uint64_t ime_serial = 0;

std::u16string guest_u16(Memory& m, uint32_t va, uint32_t max) {
  std::u16string s;
  for (uint32_t i = 0; va && i < max; i++) {
    char16_t ch = m.read<uint16_t>(va + 2 * i);
    if (!ch) break;
    s.push_back(ch);
  }
  return s;
}

std::string utf8(const std::u16string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
    if (cp < 0x80) o += char(cp);
    else if (cp < 0x800) o += char(0xC0 | cp >> 6), o += char(0x80 | (cp & 0x3F));
    else if (cp < 0x10000) o += char(0xE0 | cp >> 12), o += char(0x80 | (cp >> 6 & 0x3F)), o += char(0x80 | (cp & 0x3F));
    else
      o += char(0xF0 | cp >> 18), o += char(0x80 | (cp >> 12 & 0x3F)), o += char(0x80 | (cp >> 6 & 0x3F)),
          o += char(0x80 | (cp & 0x3F));
  }
  return o;
}

void ImeDialogInit(Cpu& c) {
  uint32_t p = arg(c, 0);
  if (!p) return ret(c, ERROR_NULL);
  std::lock_guard lk(dialog_lock);
  if (uint32_t e = begin(IME)) return ret(c, e);
  Memory& m = c.mem();
  ImeState& s = active.ime;
  s.max = std::min<uint32_t>(m.read<uint32_t>(p + 0x2C), 2048);  // SCE_IME_DIALOG_MAX_TEXT_LENGTH
  s.type = m.read<uint32_t>(p + 0x14);
  s.option = m.read<uint32_t>(p + 0x18);
  s.cancelable = m.read<uint32_t>(p + 0x20) == 1;
  s.title = guest_u16(m, m.read<uint32_t>(p + 0x28), 128);  // SCE_IME_DIALOG_MAX_TITLE_LENGTH
  s.initial = guest_u16(m, m.read<uint32_t>(p + 0x30), s.max);
  s.buffer = m.read<uint32_t>(p + 0x34);
  s.id = ++ime_serial;
  s.text = s.initial;
  active.result = RESULT_OK;
  active.button = IME_BUTTON_ENTER;
  active.waits = ime_host;
  if (!ime_host && s.text.empty()) s.text = u"Player";
  HLE_NOTE("[dialog] text input #%llu \"%s\" (max %u, type %u, option 0x%x): initial \"%s\"%s\n",
           (unsigned long long)s.id, utf8(s.title).c_str(), s.max, s.type, s.option, utf8(s.initial).c_str(),
           ime_host ? "" : ", entered as is (no window)");
  ret(c, 0);
}
void ImeDialogGetStatus(Cpu& c) {
  std::lock_guard lk(dialog_lock);
  uint32_t st = status(IME);
  ImeState& s = active.ime;
  if (st == STATUS_FINISHED && s.buffer && !s.written) {  // the text goes into the game's buffer once, on finishing
    for (size_t i = 0; i < s.text.size(); i++) c.mem().write<uint16_t>(s.buffer + 2 * uint32_t(i), uint16_t(s.text[i]));
    c.mem().write<uint16_t>(s.buffer + 2 * uint32_t(s.text.size()), 0);
    s.written = true;
  }
  ret(c, st);
}
// SceImeDialogResult {result, button (1 CLOSE, 2 ENTER), reserved[28]}
void ImeDialogGetResult(Cpu& c) {
  uint32_t r = arg(c, 0);
  if (!r) return ret(c, ERROR_NULL);
  std::lock_guard lk(dialog_lock);
  if (active.kind != IME || active.status != STATUS_FINISHED) return ret(c, ERROR_NOT_FINISHED);
  std::memset(c.mem().ptr(r), 0, 0x24);
  c.mem().write<int32_t>(r, active.result);
  c.mem().write<int32_t>(r + 4, active.button);
  ret(c, 0);
}
void ImeDialogAbort(Cpu& c) {
  std::lock_guard lk(dialog_lock);
  if (active.kind != IME || active.status == STATUS_NONE) return ret(c, ERROR_NOT_IN_USE);
  if (active.status == STATUS_RUNNING) {
    active.ime.text = active.ime.initial;
    active.result = RESULT_ABORTED;
    active.button = IME_BUTTON_NONE;
    HLE_NOTE("[dialog] text input #%llu aborted by the game\n", (unsigned long long)active.ime.id);
  }
  active.waits = false;
  active.status = STATUS_FINISHED;
  ret(c, 0);
}
void ImeDialogTerm(Cpu& c) {
  std::lock_guard lk(dialog_lock);
  ret(c, term(IME));
}

// --- trophy setup dialog: always succeeds --------------------------------------------------------------------------
void TrophySetupDialogInit(Cpu& c) {
  if (!arg(c, 0)) return ret(c, ERROR_NULL);
  std::lock_guard lk(dialog_lock);
  ret(c, begin(TROPHY));
}
void TrophySetupDialogGetStatus(Cpu& c) {
  std::lock_guard lk(dialog_lock);
  ret(c, status(TROPHY));
}
// SceNpTrophySetupDialogResult {result, reserved[128]}
void TrophySetupDialogGetResult(Cpu& c) {
  uint32_t r = arg(c, 0);
  if (!r) return ret(c, ERROR_NULL);
  std::lock_guard lk(dialog_lock);
  if (active.kind != TROPHY || active.status != STATUS_FINISHED) return ret(c, ERROR_NOT_FINISHED);
  std::memset(c.mem().ptr(r), 0, 0x84);
  ret(c, 0);
}
void TrophySetupDialogTerm(Cpu& c) {
  std::lock_guard lk(dialog_lock);
  ret(c, term(TROPHY));
}

}  // namespace

void ime_set_host(bool present) {
  std::lock_guard lk(dialog_lock);
  ime_host = present;
}

bool ime_poll(ImeRequest& out) {
  std::lock_guard lk(dialog_lock);
  if (active.kind != IME || active.status != STATUS_RUNNING || !active.waits) return false;
  const ImeState& s = active.ime;
  out.id = s.id;
  out.title = s.title;
  out.text = s.initial;
  out.max_length = s.max;
  out.type = s.type;
  out.option = s.option;
  out.cancelable = s.cancelable;
  return true;
}

void ime_answer(uint64_t id, bool ok, const std::u16string& text) {
  std::lock_guard lk(dialog_lock);
  if (active.kind != IME || active.status != STATUS_RUNNING || active.ime.id != id) return;  // aborted meanwhile
  ImeState& s = active.ime;
  if (!ok && !s.cancelable) return;
  s.text = ok ? text.substr(0, s.max) : s.initial;
  active.result = ok ? RESULT_OK : RESULT_USER_CANCELED;
  active.button = ok ? IME_BUTTON_ENTER : IME_BUTTON_CLOSE;
  active.waits = false;
  active.status = STATUS_FINISHED;
  HLE_NOTE("[dialog] text input #%llu %s: \"%s\"\n", (unsigned long long)id, ok ? "entered" : "cancelled",
           utf8(s.text).c_str());
}

void register_dialog() {
  REG(sceCommonDialogUpdate, Update);
  REG(sceMsgDialogInit, MsgDialogInit);
  REG(sceMsgDialogGetStatus, MsgDialogGetStatus);
  REG(sceMsgDialogGetResult, MsgDialogGetResult);
  REG(sceMsgDialogTerm, MsgDialogTerm);
  REG(sceSaveDataDialogInit, SaveDataDialogInit);
  REG(sceSaveDataDialogContinue, SaveDataDialogContinue);
  REG(sceSaveDataDialogFinish, SaveDataDialogFinish);
  REG(sceSaveDataDialogGetStatus, SaveDataDialogGetStatus);
  REG(sceSaveDataDialogGetSubStatus, SaveDataDialogGetSubStatus);
  REG(sceSaveDataDialogTerm, SaveDataDialogTerm);
  REG(sceNetCheckDialogInit, NetCheckDialogInit);
  REG(sceNetCheckDialogGetStatus, NetCheckDialogGetStatus);
  REG(sceNetCheckDialogGetResult, NetCheckDialogGetResult);
  REG(sceNetCheckDialogTerm, NetCheckDialogTerm);
  REG(sceImeDialogInit, ImeDialogInit);
  REG(sceImeDialogGetStatus, ImeDialogGetStatus);
  REG(sceImeDialogGetResult, ImeDialogGetResult);
  REG(sceImeDialogAbort, ImeDialogAbort);
  REG(sceImeDialogTerm, ImeDialogTerm);
  REG(sceNpTrophySetupDialogInit, TrophySetupDialogInit);
  REG(sceNpTrophySetupDialogGetStatus, TrophySetupDialogGetStatus);
  REG(sceNpTrophySetupDialogGetResult, TrophySetupDialogGetResult);
  REG(sceNpTrophySetupDialogTerm, TrophySetupDialogTerm);
}

}  // namespace hle
