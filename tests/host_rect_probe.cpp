// Finds the viewport host's WS_CHILD window (parented inside the editor's Chromium window)
// and prints its rect relative to the parent client area, plus the parent window rect.
#include <windows.h>
#include <cstdio>

struct Ctx {
    DWORD host_pid;
    DWORD editor_pid;
    HWND host_child;
    RECT host_rect{};
    int scanned;
};

static BOOL CALLBACK on_child(HWND hwnd, LPARAM lp) {
    Ctx* ctx = reinterpret_cast<Ctx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    ctx->scanned++;
    if (pid != ctx->host_pid) return TRUE;
    wchar_t class_name[64] = {};
    GetClassNameW(hwnd, class_name, 64);
    GetWindowRect(hwnd, &ctx->host_rect);
    wprintf(L"HOST_CHILD hwnd=%p %ldx%ld @ (%ld,%ld) class=%ls\n",
            hwnd, ctx->host_rect.right - ctx->host_rect.left,
            ctx->host_rect.bottom - ctx->host_rect.top,
            ctx->host_rect.left, ctx->host_rect.top, class_name);
    ctx->host_child = hwnd;
    return TRUE;
}

static BOOL CALLBACK on_top(HWND hwnd, LPARAM lp) {
    Ctx* ctx = reinterpret_cast<Ctx*>(lp);
    if (ctx->host_child) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->editor_pid) return TRUE;
    wchar_t class_name[64] = {};
    GetClassNameW(hwnd, class_name, 64);
    if (wcsstr(class_name, L"Chrome") == nullptr) return TRUE; // Chromium widget windows
    EnumChildWindows(hwnd, on_child, reinterpret_cast<LPARAM>(ctx));
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: rect_probe <host_pid> <editor_pid>\n"); return 2; }
    Ctx ctx{ static_cast<DWORD>(_wtoi(argv[1])), static_cast<DWORD>(_wtoi(argv[2])), nullptr, {}, 0 };
    EnumWindows(on_top, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.host_child) wprintf(L"HOST_CHILD none (scanned=%d)\n", ctx.scanned);
    return ctx.host_child ? 0 : 1;
}
