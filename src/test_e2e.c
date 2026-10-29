/* 端到端测试：拖动选中文字 -> 自动复制；双击空白 -> 自动粘贴 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static void MoveAbs(int x, int y)
{
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)((double)x * 65535.0 / (GetSystemMetrics(SM_CXSCREEN) - 1));
    in.mi.dy = (LONG)((double)y * 65535.0 / (GetSystemMetrics(SM_CYSCREEN) - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &in, sizeof(INPUT));
    Sleep(12);
}

static void Btn(DWORD f)
{
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = f;
    SendInput(1, &in, sizeof(INPUT));
    Sleep(12);
}

static void SetClip(const wchar_t *s)
{
    int n = (int)wcslen(s);
    HGLOBAL h;
    wchar_t *p;

    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    h = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(wchar_t));
    if (h) {
        p = (wchar_t *)GlobalLock(h);
        if (p) {
            memcpy(p, s, (n + 1) * sizeof(wchar_t));
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
    }
    CloseClipboard();
}

static void GetClip(wchar_t *buf, int cch)
{
    HANDLE h;
    wchar_t *p;

    buf[0] = 0;
    if (!OpenClipboard(NULL)) return;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        p = (wchar_t *)GlobalLock(h);
        if (p) {
            lstrcpynW(buf, p, cch);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

int main(void)
{
    HWND hWin = NULL, hEdit;
    RECT r;
    wchar_t clip[512], content[512];
    FILE *f;
    int i, okCopy, okPaste, changed;

    for (i = 0; i < 60; i++) {
        hWin = FindWindowW(L"AutoClipTestTarget", NULL);
        if (hWin) break;
        Sleep(100);
    }

    f = fopen("e2e_result.txt", "w");
    if (!hWin) {
        if (f) { fprintf(f, "FAIL: target window not found\n"); fclose(f); }
        return 1;
    }
    hEdit = FindWindowExW(hWin, NULL, L"EDIT", NULL);
    if (!hEdit) {
        if (f) { fprintf(f, "FAIL: edit control not found\n"); fclose(f); }
        return 1;
    }

    SetForegroundWindow(hWin);
    Sleep(400);
    GetWindowRect(hEdit, &r);

    /* ---------- 1) 拖选 -> 自动复制 ---------- */
    SetClip(L"MARKER_0000");
    Sleep(100);
    MoveAbs(r.left + 30, r.top + 13);
    Sleep(120);
    Btn(MOUSEEVENTF_LEFTDOWN);
    MoveAbs(r.left + 62, r.top + 13);
    MoveAbs(r.left + 108, r.top + 14);
    MoveAbs(r.left + 152, r.top + 14);
    Btn(MOUSEEVENTF_LEFTUP);
    Sleep(900);

    GetClip(clip, 512);
    okCopy = (lstrcmpW(clip, L"MARKER_0000") != 0 && clip[0] != 0);

    /* ---------- 2) 双击空行 -> 自动粘贴 ---------- */
    SetClip(L"ZZZ");
    Sleep(100);
    MoveAbs(r.left + 120, r.top + 48);
    Sleep(80);
    Btn(MOUSEEVENTF_LEFTDOWN);
    Btn(MOUSEEVENTF_LEFTUP);
    Sleep(55);
    Btn(MOUSEEVENTF_LEFTDOWN);
    Btn(MOUSEEVENTF_LEFTUP);
    Sleep(900);

    SendMessageW(hEdit, WM_GETTEXT, 512, (LPARAM)content);
    okPaste = (wcsstr(content, L"ZZZ") != NULL);

    changed = (lstrcmpW(clip, L"MARKER_0000") != 0);

    if (f) {
        fprintf(f, "copy_test   : %s  (clipboard after drag = \"%ls\")\n",
                okCopy ? "PASS" : "FAIL", clip);
        fprintf(f, "paste_test  : %s  (edit content = \"%ls\")\n",
                okPaste ? "PASS" : "FAIL", content);
        fprintf(f, "clip_changed: %d\n", changed);
        fclose(f);
    }

    PostMessageW(hWin, WM_CLOSE, 0, 0);
    return okCopy ? 0 : 2;
}
