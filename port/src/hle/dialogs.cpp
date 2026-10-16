// CommonDialog family (ErrorDialog, MsgDialog, SaveDataDialog, SigninDialog): headless. Every dialog shares one
// status machine NONE -> INITIALIZED (Initialize) -> RUNNING (Open) -> FINISHED (first status poll after Open)
// -> INITIALIZED (Close) -> NONE (Terminate). The result is "OK / YES" (buttonId 1). What the dialog would have shown
// (message text, system message type, error code) is logged so a later UI layer knows what to draw.
#include <cstring>
#include <mutex>

#include "common.h"

namespace dcp::hle {
namespace {

enum Status : int32_t { kNone = 0, kInitialized = 1, kRunning = 2, kFinished = 3 };
constexpr int32_t kButtonYes = 1;  // SCE_*_DIALOG_BUTTON_ID_OK / YES

struct Dialog {
    explicit Dialog(const char* n) : name(n) {}
    const char* name;
    std::mutex mu;
    Status status = kNone;
    int32_t opens = 0;

    int32_t init() {
        std::lock_guard<std::mutex> lk(mu);
        if (status == kNone) status = kInitialized;
        return kOk;
    }
    int32_t open() {
        std::lock_guard<std::mutex> lk(mu);
        status = kRunning;
        ++opens;
        return kOk;
    }
    // A poll finishes the dialog: the game sees RUNNING from Open's return path and FINISHED on its next poll.
    int32_t poll() {
        std::lock_guard<std::mutex> lk(mu);
        if (status == kRunning) status = kFinished;
        return status;
    }
    int32_t close() {
        std::lock_guard<std::mutex> lk(mu);
        if (status != kNone) status = kInitialized;
        return kOk;
    }
    int32_t term() {
        std::lock_guard<std::mutex> lk(mu);
        status = kNone;
        return kOk;
    }
};

Dialog g_error{"ErrorDialog"}, g_msg{"MsgDialog"}, g_save{"SaveDataDialog"}, g_signin{"SigninDialog"};

template <class T> T rd(const uint8_t* p, size_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}
const char* str_or(const char* s) { return s ? s : "(none)"; }

// Result layout shared by MsgDialog / SaveDataDialog: +0 mode, +4 result, +8 buttonId.
void fill_result(uint8_t* r, int32_t mode, size_t clear) {
    std::memset(r, 0, clear);
    std::memcpy(r, &mode, 4);
    std::memcpy(r + 8, &kButtonYes, 4);
}

// ---- CommonDialog ----
DCP_SYSV int32_t sceCommonDialogInitialize() { DCP_HLE_LOG("sceCommonDialogInitialize", "ok"); return kOk; }
DCP_SYSV int32_t sceCommonDialogIsUsed() { return 0; }

// ---- ErrorDialog: param +0 size, +4 errorCode, +8 userId ----
DCP_SYSV int32_t sceErrorDialogInitialize() { return g_error.init(); }
DCP_SYSV int32_t sceErrorDialogOpen(const uint8_t* p) {
    std::fprintf(stderr, "HLE sceErrorDialogOpen: error code 0x%08x (user %d) -> shown headless, OK\n",
                 p ? rd<uint32_t>(p, 4) : 0, p ? rd<int32_t>(p, 8) : 0);
    return g_error.open();
}
DCP_SYSV int32_t sceErrorDialogUpdateStatus() { return g_error.poll(); }
DCP_SYSV int32_t sceErrorDialogGetStatus() { return g_error.poll(); }
DCP_SYSV int32_t sceErrorDialogClose() { return g_error.close(); }
DCP_SYSV int32_t sceErrorDialogTerminate() { return g_error.term(); }

// ---- MsgDialog: param +0x38 mode (1 user msg, 2 progress bar, 3 system msg), +0x40 userMsg*, +0x48 progBar*,
// +0x50 sysMsg*; userMsg +0 buttonType, +8 msg*; progBar +8 msg*; sysMsg +0 type ----
DCP_SYSV int32_t sceMsgDialogInitialize() { return g_msg.init(); }
DCP_SYSV int32_t sceMsgDialogOpen(const uint8_t* p) {
    int32_t mode = p ? rd<int32_t>(p, 0x38) : 0;
    const uint8_t* user = p ? rd<const uint8_t*>(p, 0x40) : nullptr;
    const uint8_t* bar = p ? rd<const uint8_t*>(p, 0x48) : nullptr;
    const uint8_t* sys = p ? rd<const uint8_t*>(p, 0x50) : nullptr;
    if (mode == 1 && user)
        std::fprintf(stderr, "HLE sceMsgDialogOpen: user msg (buttons %d) \"%s\" -> OK\n", rd<int32_t>(user, 0),
                     str_or(rd<const char*>(user, 8)));
    else if (mode == 2 && bar)
        std::fprintf(stderr, "HLE sceMsgDialogOpen: progress bar \"%s\"\n", str_or(rd<const char*>(bar, 8)));
    else if (mode == 3 && sys)
        std::fprintf(stderr, "HLE sceMsgDialogOpen: system msg type %d -> OK\n", rd<int32_t>(sys, 0));
    else std::fprintf(stderr, "HLE sceMsgDialogOpen: mode %d\n", mode);
    return g_msg.open();
}
DCP_SYSV int32_t sceMsgDialogUpdateStatus() { return g_msg.poll(); }
DCP_SYSV int32_t sceMsgDialogGetStatus() { return g_msg.poll(); }
DCP_SYSV int32_t sceMsgDialogGetResult(uint8_t* r) {
    if (r) fill_result(r, 1, 0x2c);  // SceMsgDialogResult: mode, result, buttonId, reserved[32]
    return kOk;
}
DCP_SYSV int32_t sceMsgDialogClose() { return g_msg.close(); }
DCP_SYSV int32_t sceMsgDialogTerminate() { return g_msg.term(); }

// ---- SaveDataDialog: param +0x34 mode, +0x38 dispType, +0x50 userMsg* (+8 msg*), +0x58 sysMsg* (+0 type),
// +0x60 errorCode* (+0 code) ----
int32_t g_save_mode = 0;
DCP_SYSV int32_t sceSaveDataDialogInitialize() { return g_save.init(); }
DCP_SYSV int32_t sceSaveDataDialogOpen(const uint8_t* p) {
    int32_t mode = p ? rd<int32_t>(p, 0x34) : 0, disp = p ? rd<int32_t>(p, 0x38) : 0;
    const uint8_t* user = p ? rd<const uint8_t*>(p, 0x50) : nullptr;
    const uint8_t* sys = p ? rd<const uint8_t*>(p, 0x58) : nullptr;
    const uint8_t* err = p ? rd<const uint8_t*>(p, 0x60) : nullptr;
    g_save_mode = mode;
    std::fprintf(stderr, "HLE sceSaveDataDialogOpen: mode %d dispType %d", mode, disp);
    if (user) std::fprintf(stderr, " user msg \"%s\"", str_or(rd<const char*>(user, 8)));
    if (sys) std::fprintf(stderr, " sys msg type %d", rd<int32_t>(sys, 0));
    if (err) std::fprintf(stderr, " error 0x%08x", rd<uint32_t>(err, 0));
    std::fprintf(stderr, " -> OK\n");
    return g_save.open();
}
DCP_SYSV int32_t sceSaveDataDialogUpdateStatus() { return g_save.poll(); }
DCP_SYSV int32_t sceSaveDataDialogGetStatus() { return g_save.poll(); }
DCP_SYSV int32_t sceSaveDataDialogIsReadyToDisplay() { return 1; }
DCP_SYSV int32_t sceSaveDataDialogGetResult(uint8_t* r) {
    // SceSaveDataDialogResult: +0 mode, +4 result, +8 buttonId, +0x10 dirName*, +0x18 param*, +0x20 userData*.
    // Only the first 12 bytes are written: the pointers belong to the caller.
    if (r) {
        std::memcpy(r, &g_save_mode, 4);
        std::memset(r + 4, 0, 4);
        std::memcpy(r + 8, &kButtonYes, 4);
    }
    return kOk;
}
DCP_SYSV int32_t sceSaveDataDialogClose(const void*) { return g_save.close(); }
DCP_SYSV int32_t sceSaveDataDialogTerminate() { return g_save.term(); }

// ---- SigninDialog: the local user is always signed in ----
DCP_SYSV int32_t sceSigninDialogInitialize() { return g_signin.init(); }
DCP_SYSV int32_t sceSigninDialogOpen(const void*) {
    std::fprintf(stderr, "HLE sceSigninDialogOpen -> OK (headless)\n");
    return g_signin.open();
}
DCP_SYSV int32_t sceSigninDialogUpdateStatus() { return g_signin.poll(); }
DCP_SYSV int32_t sceSigninDialogTerminate() { return g_signin.term(); }

}  // namespace

void register_dialogs() {
    REG("libSceCommonDialog", sceCommonDialogInitialize); REG("libSceCommonDialog", sceCommonDialogIsUsed);
    const char* e = "libSceErrorDialog";
    REG(e, sceErrorDialogInitialize); REG(e, sceErrorDialogOpen); REG(e, sceErrorDialogUpdateStatus);
    REG(e, sceErrorDialogGetStatus); REG(e, sceErrorDialogClose); REG(e, sceErrorDialogTerminate);
    const char* m = "libSceMsgDialog";
    REG(m, sceMsgDialogInitialize); REG(m, sceMsgDialogOpen); REG(m, sceMsgDialogUpdateStatus);
    REG(m, sceMsgDialogGetStatus); REG(m, sceMsgDialogGetResult); REG(m, sceMsgDialogClose); REG(m, sceMsgDialogTerminate);
    const char* s = "libSceSaveDataDialog";
    REG(s, sceSaveDataDialogInitialize); REG(s, sceSaveDataDialogOpen); REG(s, sceSaveDataDialogUpdateStatus);
    REG(s, sceSaveDataDialogGetStatus); REG(s, sceSaveDataDialogIsReadyToDisplay); REG(s, sceSaveDataDialogGetResult);
    REG(s, sceSaveDataDialogClose); REG(s, sceSaveDataDialogTerminate);
    const char* g = "libSceSigninDialog";
    REG(g, sceSigninDialogInitialize); REG(g, sceSigninDialogOpen); REG(g, sceSigninDialogUpdateStatus);
    REG(g, sceSigninDialogTerminate);
}

}  // namespace dcp::hle
