#include "Trash.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace Slic3r::ModelLibrary {
bool move_to_trash(const std::filesystem::path& path)
{
#ifdef _WIN32
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return false;
    IFileOperation* operation = nullptr;
    IShellItem* item = nullptr;
    bool moved = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation)))) {
        // WANTNUKEWARNING prevents a silent permanent-delete fallback when
        // recycling is unavailable; NO_UI makes that warning fail the operation.
        if (SUCCEEDED(operation->SetOperationFlags(FOF_NO_UI | FOF_ALLOWUNDO | FOF_WANTNUKEWARNING)) &&
            SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
            SUCCEEDED(operation->DeleteItem(item, nullptr)) && SUCCEEDED(operation->PerformOperations())) {
            BOOL aborted = TRUE;
            moved = SUCCEEDED(operation->GetAnyOperationsAborted(&aborted)) && !aborted;
        }
    }
    if (item) item->Release();
    if (operation) operation->Release();
    CoUninitialize();
    return moved;
#else
    // gio implements the desktop trash specification, including mount-local
    // trash and restore metadata. Arguments are passed directly, never to a shell.
    const auto value = std::filesystem::absolute(path).native();
    char* const args[] = {const_cast<char*>("gio"), const_cast<char*>("trash"), const_cast<char*>("--"), const_cast<char*>(value.c_str()), nullptr};
    pid_t child;
    if (posix_spawn(&child, "/usr/bin/gio", nullptr, nullptr, args, environ) != 0) return false;
    int status = 0;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}
}
