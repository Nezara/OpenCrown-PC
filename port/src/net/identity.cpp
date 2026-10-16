// Online identity (identity.h). Paths are narrow (ANSI) strings like the rest of the user-dir code (getenv, ifstream).
#include "net/identity.h"

#include <windows.h>
#include <bcrypt.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "net/json.h"

namespace dcp::net {
namespace {

constexpr const char* kCodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // the join-code alphabet (no I O 0 1)

bool random_bytes(void* p, size_t n) {
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, static_cast<PUCHAR>(p), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string strip_slashes(std::string s) {
    while (s.size() > 3 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

void make_dirs(const std::string& dir) {  // mkdir -p (errors surface when the file is written)
    for (size_t i = 0; i <= dir.size(); ++i)
        if (i == dir.size() || ((dir[i] == '/' || dir[i] == '\\') && i > 2))
            CreateDirectoryA(dir.substr(0, i).c_str(), nullptr);
}

bool is_secret(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// Parses identity.ini text; false when a field is missing or malformed.
bool parse(const std::string& text, Identity* id) {
    std::istringstream in(text);
    std::string line;
    bool have_acc = false, have_sec = false;
    while (std::getline(in, line)) {
        line = trim(line.substr(0, line.find('#')));
        size_t eq = line.find('=');
        if (line.empty() || eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "account_id") {
            char* end = nullptr;
            uint64_t a = std::strtoull(v.c_str(), &end, 10);
            have_acc = !v.empty() && end && !*end && v[0] != '-' && a && (a & 1);
            id->account_id = a;
        } else if (k == "secret") {
            for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
            have_sec = is_secret(v);
            id->secret = v;
        }
    }
    return have_acc && have_sec;
}

bool read_file(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

}  // namespace

std::string identity_dir() {
    if (const char* d = std::getenv("DCP_USER_DIR"); d && *d) return strip_slashes(d);
    if (const char* r = std::getenv("DCP_SAVE_ROOT"); r && *r) {
        std::string s = strip_slashes(r);
        size_t k = s.find_last_of("/\\");
        if (k != std::string::npos && k > 0) return s.substr(0, k);
    }
    const char* home = std::getenv("USERPROFILE");
    return std::string(home ? home : ".") + "/Documents/Dragons Crown Pro";
}

Identity load_identity(const std::string& dir) {
    Identity id;
    id.path = strip_slashes(dir) + "/identity.ini";
    std::string text;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (read_file(id.path, &text)) {
            if (parse(text, &id)) {
                id.ok = true;
                return id;
            }
            id.account_id = 0, id.secret.clear();
            id.error = "malformed " + id.path + " (left as is; delete it to get a new account)";
            return id;
        }
        if (attempt) break;
        // First use: new random identity, written to a temp file and moved into place without replacing, so a
        // launcher and a game starting at the same moment cannot both create one (the loser reads the winner's).
        uint64_t acc = 0;
        uint8_t sec[32];
        if (!random_bytes(&acc, sizeof acc) || !random_bytes(sec, sizeof sec)) {
            id.error = "BCryptGenRandom failed";
            return id;
        }
        acc |= 1;  // odd = never 0 ("server assigns one")
        make_dirs(strip_slashes(dir));
        std::string tmp = id.path + ".tmp" + std::to_string(GetCurrentProcessId());
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            o << "# OpenCrown online identity. Keep it private: the secret proves this account to the lobby.\n"
                 "# Deleting this file creates a new account on the next start (friends are lost).\n"
              << "account_id = " << u64_str(acc) << "\nsecret = " << hex_encode(sec, sizeof sec) << "\n";
            if (!o) {
                id.error = "cannot write " + tmp;
                return id;
            }
        }
        SecureZeroMemory(sec, sizeof sec);
        if (!MoveFileExA(tmp.c_str(), id.path.c_str(), MOVEFILE_WRITE_THROUGH)) DeleteFileA(tmp.c_str());
    }
    id.error = "cannot create " + id.path;
    return id;
}

std::string online_name(const std::string& configured) {
    const char* n = configured.c_str();
    if (!*n) n = std::getenv("USERNAME");
    std::string s;
    for (const char* p = n ? n : ""; *p && s.size() < 16; ++p)
        if (std::isalnum(static_cast<unsigned char>(*p)) || *p == '_' || *p == '-') s += *p;
    return s.size() < 3 ? "Player" : s;
}

std::string sha256_hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    uint8_t out[32] = {};
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0)) &&
              BCRYPT_SUCCESS(BCryptHashData(h, PUCHAR(data.data()), ULONG(data.size()), 0)) &&
              BCRYPT_SUCCESS(BCryptFinishHash(h, out, sizeof out, 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok ? hex_encode(out, sizeof out) : "";
}

std::string friend_code_normalize(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '-' || c == ' ' || c == '\t') continue;
        c = char(std::toupper(static_cast<unsigned char>(c)));
        if (!std::strchr(kCodeAlphabet, c) || !c) return "";
        r += c;
    }
    return r.size() == 8 ? r : "";
}

std::string friend_code_display(const std::string& code8) {
    return code8.size() == 8 ? code8.substr(0, 4) + "-" + code8.substr(4) : code8;
}

}  // namespace dcp::net
