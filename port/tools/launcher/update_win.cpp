// Windows side of the launcher updater (update.h): WinHTTP with hand-followed redirects (every hop must pass
// url_allowed), BCrypt SHA-256, tar.exe extraction and the rename / copy / rollback install.
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "update.h"

namespace dcp::update {

namespace {

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wnative(const std::string& p) {
    std::wstring w = widen(p);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    while (w.size() > 3 && w.back() == L'\\') w.pop_back();
    return w;
}

bool wexists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool wis_dir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string win_error(DWORD e) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
                   0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string s = buf ? narrow(buf) : "";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.' || s.back() == ' ')) s.pop_back();
    return s.empty() ? "error " + std::to_string(e) : s + " (" + std::to_string(e) + ")";
}

// Deletes a folder tree; reparse points (links) are removed, never followed.
void remove_tree(const std::wstring& dir) {
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (a & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(dir.c_str());
        else DeleteFileW(dir.c_str());
        return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n != L"." && n != L"..") remove_tree(dir + L"\\" + n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// Regular files under dir (relative paths), links skipped.
void list_files(const std::wstring& dir, const std::wstring& rel, std::vector<std::wstring>* out, int depth) {
    if (depth > 8) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + (rel.empty() ? L"" : L"\\" + rel) + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        std::wstring r = rel.empty() ? n : rel + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) list_files(dir, r, out, depth + 1);
        else out->push_back(r);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Folder (under root, up to 4 levels deep) holding a file named `name` (case-insensitive), or L"".
std::wstring find_dir_with(const std::wstring& root, const std::wstring& name, int depth) {
    if (wexists(root + L"\\" + name) && !wis_dir(root + L"\\" + name)) return root;
    if (depth >= 4) return L"";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        found = find_dir_with(root + L"\\" + n, name, depth + 1);
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

bool make_parents(const std::wstring& root, const std::wstring& rel) {
    size_t p = 0;
    while ((p = rel.find(L'\\', p)) != std::wstring::npos) {
        std::wstring d = root + L"\\" + rel.substr(0, p);
        if (!wis_dir(d) && !CreateDirectoryW(d.c_str(), nullptr)) return false;
        ++p;
    }
    return true;
}

class Sha256 {
public:
    Sha256() {
        ok_ = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg_, &h_, nullptr, 0, nullptr, 0, 0));
    }
    ~Sha256() {
        if (h_) BCryptDestroyHash(h_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    void update(const void* p, size_t n) {
        const auto* b = static_cast<const UCHAR*>(p);
        while (ok_ && n) {
            ULONG k = ULONG(std::min<size_t>(n, 1u << 30));
            ok_ = BCRYPT_SUCCESS(BCryptHashData(h_, const_cast<PUCHAR>(b), k, 0));
            b += k, n -= k;
        }
    }
    std::string hex() {
        UCHAR d[32];
        if (!ok_ || !BCRYPT_SUCCESS(BCryptFinishHash(h_, d, sizeof d, 0))) return "";
        static const char* k = "0123456789abcdef";
        std::string s;
        for (UCHAR c : d) s += k[c >> 4], s += k[c & 15];
        return s;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE h_ = nullptr;
    bool ok_ = false;
};

struct Handles {
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    void close_req() {
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        req = con = nullptr;
    }
    ~Handles() {
        close_req();
        if (ses) WinHttpCloseHandle(ses);
    }
};

using Sink = std::function<bool(const char*, size_t)>;  // false = stop reading

// GET url, following up to 8 redirects by hand; every address must pass url_allowed. The final response body goes
// to sink. Returns the final status (0 + error = no answer, or a refused / failed hop).
HttpResult request(const std::string& url0, const std::string& ua, const std::string& accept, bool test, const Sink& sink,
                   const std::atomic<bool>* cancel) {
    HttpResult r;
    Handles h;
    h.ses = WinHttpOpen(widen(ua).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.ses) return r.error = "WinHTTP unavailable", r;
    WinHttpSetTimeouts(h.ses, 10000, 10000, 15000, 30000);
    std::string url = url0;
    for (int hop = 0; hop < 9; ++hop) {
        if (!url_allowed(url, test)) return r.error = "refused to download from " + parse_url(url).host, r;
        Url u = parse_url(url);
        h.close_req();
        h.con = WinHttpConnect(h.ses, widen(u.host).c_str(), INTERNET_PORT(u.port), 0);
        h.req = h.con ? WinHttpOpenRequest(h.con, L"GET", widen(u.rest).c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, u.scheme == "https" ? WINHTTP_FLAG_SECURE : 0)
                      : nullptr;
        if (!h.req) return r.error = "can't reach " + u.host, r;
        DWORD pol = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(h.req, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof pol);
        std::wstring hdr = widen("Accept: " + accept + "\r\n");
        if (!WinHttpSendRequest(h.req, hdr.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(h.req, nullptr)) {
            DWORD e = GetLastError();
            return r.error = "can't reach " + u.host + (e == ERROR_WINHTTP_TIMEOUT ? " (timed out)" : ""), r;
        }
        DWORD st = 0, len = sizeof st;
        WinHttpQueryHeaders(h.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &st, &len, WINHTTP_NO_HEADER_INDEX);
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
            DWORD n = 0;
            WinHttpQueryHeaders(h.req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &n,
                                WINHTTP_NO_HEADER_INDEX);
            std::wstring loc(n / sizeof(wchar_t) + 1, L'\0');
            if (!n || !WinHttpQueryHeaders(h.req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc.data(), &n,
                                           WINHTTP_NO_HEADER_INDEX))
                return r.status = int(st), r.error = "redirect without a location", r;
            loc.resize(n / sizeof(wchar_t));
            std::string next = narrow(loc);
            if (!next.empty() && next[0] == '/') next = u.scheme + "://" + u.host + ":" + std::to_string(u.port) + next;
            url = next;
            continue;
        }
        r.status = int(st), r.final_url = url;
        for (;;) {
            if (cancel && cancel->load()) return r.error = "cancelled", r.status = 0, r;
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(h.req, &avail)) return r.error = "connection lost", r.status = 0, r;
            if (avail == 0) break;
            std::string chunk(std::min<DWORD>(avail, 1u << 20), '\0');
            DWORD got = 0;
            if (!WinHttpReadData(h.req, chunk.data(), DWORD(chunk.size()), &got)) return r.error = "connection lost", r.status = 0, r;
            if (got == 0) break;
            if (!sink(chunk.data(), got)) break;
        }
        return r;
    }
    return r.error = "too many redirects", r;
}

const wchar_t kStaging[] = L"update_staging";
const wchar_t kManifest[] = L"update_old.lst";

}  // namespace

HttpResult http_get(const std::string& url, const std::string& ua, const std::string& accept, bool test, size_t max_body) {
    std::string body;
    bool over = false;
    HttpResult r = request(url, ua, accept, test, [&](const char* p, size_t n) {
        if (body.size() + n > max_body) return over = true, false;
        body.append(p, n);
        return true;
    }, nullptr);
    if (over) return r.status = 0, r.error = "answer too large", r;
    r.body = std::move(body);
    return r;
}

std::string sha256_hex(const void* p, size_t n) {
    Sha256 h;
    h.update(p, n);
    return h.hex();
}

std::string sha256_file(const std::string& path) {
    HANDLE f = CreateFileW(wnative(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    Sha256 h;
    std::vector<char> buf(1 << 20);
    DWORD got = 0;
    bool ok = true;
    while ((ok = ReadFile(f, buf.data(), DWORD(buf.size()), &got, nullptr)) && got) h.update(buf.data(), got);
    CloseHandle(f);
    return ok ? h.hex() : "";
}

bool download_verified(const std::string& url, const std::string& ua, bool test, const std::string& path,
                       int64_t want_size, const std::string& want_sha, Progress* prog, std::string* err) {
    if (want_sha.size() != 64) return *err = "the release gives no checksum; install it by hand", false;
    const std::wstring wpath = wnative(path);
    HANDLE f = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return *err = "cannot write " + path + ": " + win_error(GetLastError()), false;
    Sha256 h;
    int64_t got = 0;
    bool too_big = false, wfail = false;
    prog->total = want_size, prog->done = 0;
    HttpResult r = request(url, ua, "application/octet-stream", test, [&](const char* p, size_t n) {
        if (got + int64_t(n) > want_size) return too_big = true, false;
        DWORD w = 0;
        if (!WriteFile(f, p, DWORD(n), &w, nullptr) || w != n) return wfail = true, false;
        h.update(p, n);
        got += int64_t(n);
        prog->done = got;
        return true;
    }, &prog->cancel);
    CloseHandle(f);
    std::string sha = h.hex();
    auto fail = [&](const std::string& m) {
        DeleteFileW(wpath.c_str());
        *err = m;
        return false;
    };
    if (r.status == 0) return fail("download failed: " + r.error);
    if (r.status != 200) return fail("download failed: HTTP " + std::to_string(r.status));
    if (wfail) return fail("could not write the download (disk full?)");
    if (too_big || got != want_size)
        return fail("download size mismatch (" + std::to_string(got) + " of " + std::to_string(want_size) +
                    " bytes); nothing was changed");
    if (sha != want_sha) return fail("checksum mismatch: the download is damaged; nothing was changed");
    return true;
}

std::string exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    std::wstring w(buf, n);
    return narrow(w.substr(0, w.find_last_of(L"\\/")));
}

bool dir_writable(const std::string& dir) {
    std::wstring p = wnative(dir) + L"\\.opencrown_write_test";
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    CloseHandle(f);
    return true;
}

bool install_zip(const std::string& zip, const std::string& dir, std::string* err) {
    const std::wstring root = wnative(dir), staging = root + L"\\" + kStaging;
    remove_tree(staging);
    if (wexists(staging) || !CreateDirectoryW(staging.c_str(), nullptr))
        return *err = "cannot create the update_staging folder: " + win_error(GetLastError()), false;
    auto fail = [&](const std::string& m) {
        remove_tree(staging);
        *err = m;
        return false;
    };
    // Unpack with Windows' own tar.exe (bsdtar; reads zip; refuses absolute and .. paths).
    wchar_t sys[MAX_PATH];
    UINT sn = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = std::wstring(sys, sn) + L"\\tar.exe";
    if (!sn || !wexists(tar)) return fail("tar.exe not found in System32 (Windows 10 1803 or newer is needed)");
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + wnative(zip) + L"\" -C \"" + staging + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, staging.c_str(), &si, &pi))
        return fail("cannot run tar.exe: " + win_error(GetLastError()));
    CloseHandle(pi.hThread);
    DWORD wr = WaitForSingleObject(pi.hProcess, 180000), code = 1;
    if (wr != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1), WaitForSingleObject(pi.hProcess, 5000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (wr != WAIT_OBJECT_0 || code != 0) return fail("could not unpack the update (tar exit code " + std::to_string(code) + ")");

    const std::wstring payload = find_dir_with(staging, widen(kLauncherExe), 0);
    if (payload.empty()) return fail("the update has no " + std::string(kLauncherExe));
    for (const wchar_t* need : {L"dcp_run.exe", L"SDL3.dll"})
        if (!wexists(payload + L"\\" + need)) return fail("the update is incomplete (" + narrow(need) + " missing)");
    std::vector<std::wstring> files;
    list_files(payload, L"", &files, 0);
    // Launcher last: if anything before it fails, the running one is untouched.
    std::stable_partition(files.begin(), files.end(), [](const std::wstring& f) { return _wcsicmp(f.c_str(), widen(kLauncherExe).c_str()) != 0; });

    struct Step {
        std::wstring target;
        bool had_old;
    };
    std::vector<Step> done;
    auto rollback = [&] {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            DeleteFileW(it->target.c_str());
            if (it->had_old) MoveFileExW((it->target + L".old").c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    };
    for (const std::wstring& rel : files) {
        const std::wstring src = payload + L"\\" + rel, dst = root + L"\\" + rel, old = dst + L".old";
        if (!make_parents(root, rel)) {
            DWORD e = GetLastError();
            rollback();
            return fail("cannot create a folder for " + narrow(rel) + ": " + win_error(e));
        }
        if (wis_dir(dst)) {
            rollback();
            return fail(narrow(rel) + " is a folder in the install folder; nothing was changed");
        }
        bool had = wexists(dst);
        if (had) {
            SetFileAttributesW(old.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(old.c_str());
            if (!MoveFileExW(dst.c_str(), old.c_str(), 0)) {
                DWORD e = GetLastError();
                rollback();
                return fail("cannot replace " + narrow(rel) + ": " + win_error(e) + "; nothing was changed");
            }
        }
        done.push_back({dst, had});
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            DWORD e = GetLastError();
            rollback();
            return fail("cannot copy " + narrow(rel) + ": " + win_error(e) + "; nothing was changed");
        }
    }
    std::ofstream m(std::filesystem::path(root + L"\\" + kManifest), std::ios::app);
    for (const Step& s : done)
        if (s.had_old) m << narrow(s.target.substr(root.size() + 1)) << ".old\n";
    m.close();
    remove_tree(staging);
    return true;
}

void cleanup_leftovers(const std::string& dir, int wait_ms) {
    const std::wstring root = wnative(dir);
    remove_tree(root + L"\\" + kStaging);
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW((root + L"\\*.zip.part").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((root + L"\\" + fd.cFileName).c_str());
        while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    std::vector<std::string> olds = {std::string(kLauncherExe) + ".old", "dcp_run.exe.old", "SDL3.dll.old"};
    {
        std::ifstream m(std::filesystem::path(root + L"\\" + kManifest));
        std::string l;
        while (std::getline(m, l)) {
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            if (l.size() > 4 && l.compare(l.size() - 4, 4, ".old") == 0 && l.find("..") == std::string::npos &&
                l.find(':') == std::string::npos && l[0] != '\\' && l[0] != '/' &&
                std::find(olds.begin(), olds.end(), l) == olds.end())
                olds.push_back(l);
        }
    }
    const ULONGLONG until = GetTickCount64() + ULONGLONG(std::max(0, wait_ms));
    for (;;) {
        std::vector<std::string> left;
        for (const std::string& o : olds) {
            std::wstring p = root + L"\\" + wnative(o);
            if (!wexists(p)) continue;
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(p.c_str())) left.push_back(o);
        }
        olds.swap(left);
        if (olds.empty() || GetTickCount64() >= until) break;
        Sleep(200);
    }
    const std::wstring man = root + L"\\" + kManifest;
    if (olds.empty()) {
        DeleteFileW(man.c_str());
    } else {
        std::ofstream m(std::filesystem::path(man), std::ios::trunc);
        for (const std::string& o : olds) m << o << "\n";
    }
}

bool relaunch(const std::string& dir, const std::string& old_version, std::string* err) {
    std::wstring exe = wnative(dir) + L"\\" + widen(kLauncherExe);
    std::wstring cmd = L"\"" + exe + L"\" --updated " + widen(old_version);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return *err = "the update is installed but the new launcher did not start (" + win_error(GetLastError()) +
                      "); please start it again", false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace dcp::update
