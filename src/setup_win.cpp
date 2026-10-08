/*
 * First-run setup, Windows: asks for the player's files with the standard
 * dialogs, checks them (src/setup_iso.cpp) and copies them beside the
 * executable, with the shell's progress dialog. dod3_setup_win() returns 0
 * when everything is in place.
 */
#include "setup_iso.h"

#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")

namespace {

using namespace dod3setup;

const wchar_t kTitle[] = L"Drakengard 3 Recompiled — setup";

std::wstring widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

int ask(const std::wstring& text, UINT flags) { return MessageBoxW(NULL, text.c_str(), kTitle, flags | MB_SETFOREGROUND); }

/* One file or folder from the standard dialog; empty if cancelled. */
fs::path pick(const wchar_t* title, bool folder, const wchar_t* filter_name = NULL, const wchar_t* filter = NULL)
{
    fs::path out;
    IFileOpenDialog* d = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    d->SetTitle(title);
    if (!folder && filter) {
        COMDLG_FILTERSPEC spec[2] = { { filter_name, filter }, { L"All files", L"*.*" } };
        d->SetFileTypes(2, spec);
    }
    if (SUCCEEDED(d->Show(NULL))) {
        IShellItem* it = NULL;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    d->Release();
    return out;
}

bool copy_file_to(const fs::path& from, const fs::path& to, std::wstring* err)
{
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) { *err = L"Could not copy " + from.wstring() + L":\n" + widen(ec.message()); return false; }
    return true;
}

/* The disc: a decrypted ISO or a folder holding PS3_GAME, checked, then
 * copied to base/game/disc with a progress dialog. */
bool setup_disc(const fs::path& base)
{
    for (;;) {
        const int kind = ask(L"Step 1 of 3: the game disc.\n\n"
                             L"Is your copy of the disc a decrypted .iso file?\n\n"
                             L"Yes — choose the .iso file\n"
                             L"No — choose the folder that contains PS3_GAME (a disc dumped to a folder)",
                             MB_YESNOCANCEL | MB_ICONQUESTION);
        if (kind == IDCANCEL) return false;
        const fs::path src = kind == IDYES ? pick(L"Choose the decrypted Drakengard 3 disc image", false, L"Disc images (*.iso)", L"*.iso")
                                           : pick(L"Choose the folder that contains PS3_GAME", true);
        if (src.empty()) continue;
        Disc disc;
        std::string err;
        HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
        const bool ok = open_disc(src, &disc, &err);
        SetCursor(old);
        if (!ok) {
            if (ask(L"That disc can't be used:\n\n" + widen(err), MB_RETRYCANCEL | MB_ICONWARNING) == IDCANCEL) return false;
            continue;
        }
        ULARGE_INTEGER avail = {};
        std::error_code ec;
        fs::create_directories(base / "game", ec);
        if (GetDiskFreeSpaceExW((base / "game").c_str(), &avail, NULL, NULL) && avail.QuadPart < disc.total + (256ull << 20)) {
            ask(L"Not enough free space: the game's files need " + std::to_wstring((disc.total >> 30) + 1) +
                L" GB on the drive of\n" + base.wstring(), MB_OK | MB_ICONERROR);
            return false;
        }

        /* Copy to game/disc.partial, then rename: a cancelled or failed copy
         * never looks installed. */
        const fs::path part = base / "game/disc.partial", final_dir = base / "game/disc";
        fs::remove_all(part, ec);
        IProgressDialog* pd = NULL;
        CoCreateInstance(CLSID_ProgressDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pd));
        if (pd) {
            pd->SetTitle(kTitle);
            pd->SetLine(1, L"Copying the game's files", FALSE, NULL);
            pd->SetLine(2, (L"to " + final_dir.wstring()).c_str(), TRUE, NULL);
            pd->StartProgressDialog(NULL, NULL, PROGDLG_NORMAL | PROGDLG_AUTOTIME, NULL);
        }
        ULONGLONG last = 0;
        const bool copied = copy_disc(disc, part, [&](uint64_t done, uint64_t total) {
            if (!pd) return true;
            const ULONGLONG now = GetTickCount64();
            if (now - last >= 100) { last = now; pd->SetProgress64(done, total); }
            return !pd->HasUserCancelled();
        }, &err);
        if (pd) { pd->StopProgressDialog(); pd->Release(); }
        if (!copied) {
            fs::remove_all(part, ec);
            if (err == "cancelled") return false;
            if (ask(L"Copying the disc failed:\n\n" + widen(err), MB_RETRYCANCEL | MB_ICONWARNING) == IDCANCEL) return false;
            continue;
        }
        fs::remove_all(final_dir, ec);
        fs::rename(part, final_dir, ec);
        if (ec) { ask(L"Could not finish the copy:\n" + widen(ec.message()), MB_OK | MB_ICONERROR); return false; }
        return true;
    }
}

/* A single file checked by its hash: the decrypted EBOOT.ELF or flashMP3.pic. */
bool setup_file(const fs::path& base, const wchar_t* step, const wchar_t* title, const wchar_t* filter_name,
                const wchar_t* filter, const char* want_sha, const fs::path& dest, bool mismatch_ok)
{
    for (;;) {
        if (ask(step, MB_OKCANCEL | MB_ICONINFORMATION) == IDCANCEL) return false;
        const fs::path src = pick(title, false, filter_name, filter);
        if (src.empty()) continue;
        HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
        const std::string sha = sha256_file(src);
        SetCursor(old);
        if (sha != want_sha) {
            if (mismatch_ok) {
                const int r = ask(L"This file is not the one the game was built against (firmware 4.55's). "
                                  L"Music and voices that use MP3 may fail with it.\n\nUse it anyway?",
                                  MB_YESNOCANCEL | MB_ICONWARNING);
                if (r == IDCANCEL) return false;
                if (r == IDNO) continue;
            } else {
                if (ask(L"That file doesn't match the supported release (Drakengard 3, BLUS31197, version 01.00). "
                        L"It must be the disc's PS3_GAME\\USRDIR\\EBOOT.BIN decrypted with RPCS3 "
                        L"(Utilities › Decrypt PS3 Binaries), with no game update applied.",
                        MB_RETRYCANCEL | MB_ICONWARNING) == IDCANCEL) return false;
                continue;
            }
        }
        std::wstring err;
        if (!copy_file_to(src, base / dest, &err)) { ask(err, MB_OK | MB_ICONERROR); return false; }
        return true;
    }
}

}  // namespace

extern "C" int dod3_setup_win(const wchar_t* base_dir, int force)
{
    const fs::path base(base_dir);
    Installed have = check_installed(base);
    if (have.all() && !force) return 0;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    if (ask(L"Drakengard 3 Recompiled needs these files from your own copy of the game:\n\n"
            L"1. The game disc (Drakengard 3, US, BLUS31197): a decrypted .iso, or the disc dumped to a folder.\n"
            L"2. EBOOT.ELF: the disc's PS3_GAME\\USRDIR\\EBOOT.BIN decrypted with RPCS3 (Utilities › Decrypt PS3 Binaries).\n"
            L"3. flashMP3.pic: from PS3 firmware installed in RPCS3 (File › Install Firmware), in its "
            L"dev_flash\\sys\\external folder.\n\n"
            L"The files are copied into\n" + base.wstring() + L"\n(about 16 GB).",
            MB_OKCANCEL | MB_ICONINFORMATION) == IDCANCEL) { CoUninitialize(); return 1; }

    bool ok = true;
    if (ok && (force || !have.disc)) ok = setup_disc(base);
    if (ok && (force || !have.elf))
        ok = setup_file(base, L"Step 2 of 3: EBOOT.ELF.\n\nChoose the EBOOT.ELF that RPCS3's "
                              L"Utilities › Decrypt PS3 Binaries made from the disc's PS3_GAME\\USRDIR\\EBOOT.BIN.",
                        L"Choose the decrypted EBOOT.ELF", L"Decrypted executable (*.elf)", L"*.elf;*.ELF",
                        kEbootElfSha256, "elf/EBOOT.ELF", false);
    if (ok && (force || !have.mp3))
        ok = setup_file(base, L"Step 3 of 3: flashMP3.pic.\n\nChoose flashMP3.pic from the PS3 firmware RPCS3 "
                              L"installed: <RPCS3 folder>\\dev_flash\\sys\\external\\flashMP3.pic.",
                        L"Choose flashMP3.pic", L"flashMP3.pic", L"flashMP3.pic",
                        kFlashMp3Sha256, "fw/dev_flash/sys/external/flashMP3.pic", true);
    CoUninitialize();
    if (!ok) return 1;
    ask(L"Setup is complete. The game will start now.\n\nTo run setup again: dod3.exe --setup", MB_OK | MB_ICONINFORMATION);
    return 0;
}
