#include "common.hpp"
#include <Windows.h>
namespace codm {
void write_json(const fs::path &p, const J &j) {
    if (!p.parent_path().empty())
        fs::create_directories(p.parent_path());
    static std::atomic_uint64_t sequence = 0;
    // Keep the atomic sibling short: appending to a long material filename can
    // exceed Windows' path limit even when the final destination is valid.
    auto temporary = p.parent_path() / (L".codm-tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(sequence++));
    try {
        std::ofstream out(temporary);
        require(bool(out), "Cannot write " + pathstr(p));
        out << j.dump(2);
        out.close();
        require(bool(out), "Write failed: " + pathstr(p));
        BOOL published=FALSE;DWORD error=0;
        for(int attempt=0;attempt<8;++attempt) {
            published=MoveFileExW(temporary.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
            if(published)break;error=GetLastError();
            if(error!=ERROR_ACCESS_DENIED && error!=ERROR_SHARING_VIOLATION)break;
            Sleep(DWORD(25*(attempt+1)));
        }
        require(published != 0,
                "Cannot publish " + pathstr(p) + ": Windows error " + std::to_string(error));
    } catch (...) {
        std::error_code error;
        fs::remove(temporary, error);
        throw;
    }
}
} // namespace codm
