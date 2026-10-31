// The installer's custom actions (installer/W-Rally.wxs): finding Richard Burns Rally and checking the folder chosen.
// W-Rally has no content of its own yet, so it cannot be installed without RBR's (design 9, decision 7).
//
//   DetectRbr    RBRDIR unset or not an RBR folder: the first RBR folder found - the uninstall entries and SCi's
//                key, then the usual places and one level down every fixed drive (and its Games folder).
//   ValidateRbr  RBR_VALID = "1" when RBRDIR holds the RBR files W-Rally reads (RichardBurnsRally_SSE.exe,
//                Physics.rbz, Maps\Tracks.ini, Cars\Cars.ini), else ""; RBRPATH the
//                folder without a trailing backslash (what install.ini and the registry keep).

#include <windows.h>

#include <msi.h>
#include <msiquery.h>

#include <string>
#include <vector>

namespace {

std::wstring Property(MSIHANDLE h, const wchar_t* name)
{
    DWORD n = 0;
    wchar_t none[1] = {};
    if (MsiGetPropertyW(h, name, none, &n) != ERROR_MORE_DATA) return L"";
    std::wstring v(++n, L'\0');
    if (MsiGetPropertyW(h, name, v.data(), &n) != ERROR_SUCCESS) return L"";
    v.resize(n);
    return v;
}

std::wstring Trimmed(std::wstring dir)
{
    while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    return dir;
}

bool Exists(const std::wstring& path, bool folder)
{
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && ((a & FILE_ATTRIBUTE_DIRECTORY) != 0) == folder;
}

bool IsRbr(const std::wstring& dir)
{
    if (dir.empty()) return false;
    const std::wstring d = Trimmed(dir) + L"\\";
    // what W-Rally reads: the stages' list, the cars' list and their physics (the co-driver's voice is optional)
    for (const wchar_t* file : {L"RichardBurnsRally_SSE.exe", L"Physics.rbz", L"Maps\\Tracks.ini", L"Cars\\Cars.ini"})
        if (!Exists(d + file, false)) return false;
    return true;
}

std::wstring RegString(HKEY root, const std::wstring& key, const wchar_t* value, REGSAM view)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | view, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}

// The uninstall entries whose name says RBR (RallySimFans' installer among them): their install locations.
void FromUninstall(std::vector<std::wstring>& out)
{
    const wchar_t* path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
        for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
            HKEY k;
            if (RegOpenKeyExW(root, path, 0, KEY_READ | view, &k) != ERROR_SUCCESS) continue;
            wchar_t name[256];
            for (DWORD i = 0, n = 256; RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
                 ++i, n = 256) {
                const std::wstring sub = std::wstring(path) + L"\\" + name;
                const std::wstring shown = RegString(root, sub, L"DisplayName", view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : RRF_SUBKEY_WOW6464KEY);
                if (shown.find(L"Richard Burns") == std::wstring::npos && shown.find(L"RallySimFans") == std::wstring::npos)
                    continue;
                const DWORD flag = view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : RRF_SUBKEY_WOW6464KEY;
                out.push_back(RegString(root, sub, L"InstallLocation", flag));
                std::wstring icon = RegString(root, sub, L"DisplayIcon", flag);   // "<dir>\exe,0"
                if (const size_t slash = icon.find_last_of(L'\\'); slash != std::wstring::npos) out.push_back(icon.substr(0, slash));
            }
            RegCloseKey(k);
        }
}

// The folders one level under `dir`.
void Children(const std::wstring& dir, std::vector<std::wstring>& out)
{
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && f.cFileName[0] != L'.') out.push_back(dir + L"\\" + f.cFileName);
    } while (FindNextFileW(h, &f));
    FindClose(h);
}

std::wstring Detect()
{
    std::vector<std::wstring> c;
    FromUninstall(c);
    for (REGSAM view : {RRF_SUBKEY_WOW6432KEY, RRF_SUBKEY_WOW6464KEY})
        c.push_back(RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SCi Games\\Richard Burns Rally", L"InstallPath", view));
    wchar_t drives[256];
    const DWORD n = GetLogicalDriveStringsW(255, drives);
    for (const wchar_t* d = drives; n && n < 256 && *d; d += wcslen(d) + 1) {
        if (GetDriveTypeW(d) != DRIVE_FIXED) continue;
        const std::wstring root = Trimmed(d).substr(0, 2);   // "C:"
        for (const wchar_t* usual : {L"\\Richard Burns Rally", L"\\RBR", L"\\Games\\Richard Burns Rally", L"\\Games\\RBR",
                                     L"\\Program Files (x86)\\SCi Games\\Richard Burns Rally",
                                     L"\\Program Files (x86)\\Richard Burns Rally", L"\\Program Files\\Richard Burns Rally"})
            c.push_back(root + usual);
        Children(root, c);
        Children(root + L"\\Games", c);
    }
    for (const std::wstring& dir : c)
        if (IsRbr(dir)) return Trimmed(dir);
    return L"";
}

} // namespace

extern "C" __declspec(dllexport) UINT __stdcall DetectRbr(MSIHANDLE h)
{
    if (IsRbr(Property(h, L"RBRDIR"))) return ERROR_SUCCESS;
    if (const std::wstring found = Detect(); !found.empty()) MsiSetPropertyW(h, L"RBRDIR", (found + L"\\").c_str());
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT __stdcall ValidateRbr(MSIHANDLE h)
{
    const std::wstring dir = Property(h, L"RBRDIR");
    const bool ok = IsRbr(dir);
    MsiSetPropertyW(h, L"RBR_VALID", ok ? L"1" : L"");
    MsiSetPropertyW(h, L"RBRPATH", ok ? Trimmed(dir).c_str() : L"");
    return ERROR_SUCCESS;
}
