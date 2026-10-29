/* 测试靶子：一个带单行编辑框的小窗口，供 AutoClip 施压 */
#include <windows.h>

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR c, int s)
{
    WNDCLASSEXW wc;
    HWND hw, he;
    MSG msg;
    HFONT font;

    (void)hp; (void)c; (void)s;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.hInstance = hi;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AutoClipTestTarget";
    RegisterClassExW(&wc);

    hw = CreateWindowExW(WS_EX_TOPMOST, L"AutoClipTestTarget", L"AutoClip 测试窗口",
                         WS_OVERLAPPEDWINDOW, 160, 140, 500, 230,
                         NULL, NULL, hi, NULL);
    if (!hw) return 1;

    he = CreateWindowExW(0, L"EDIT", L"ALPHA BETA GAMMA\r\n\r\n\r\n",
                         WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_WANTRETURN,
                         12, 44, 460, 130, hw, (HMENU)100, hi, NULL);

    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (he && font) SendMessageW(he, WM_SETFONT, (WPARAM)font, TRUE);

    ShowWindow(hw, SW_SHOW);
    UpdateWindow(hw);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
