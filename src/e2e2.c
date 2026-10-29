/*
 * End-to-end check for AutoClip.
 *
 *   test copy      : drag select over "ALPHA" -> clipboard must change
 *   test paste     : double click on an empty line -> clipboard text inserted
 *   test doubleword: double click on a word with paste enabled -> the word is
 *                    selected and NOT replaced by the clipboard
 *
 * Results go to e2e_out.txt next to the exe.
 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static FILE *g_out;
static int g_fail;

static void W(const wchar_t *s)
{
    const wchar_t *p;

    if (!g_out) return;
    for (p = s; *p; p++) {
        if (*p < 128) fputc((int)*p, g_out);
        else fputc('?', g_out);
    }
}

static void Say(const wchar_t *fmt, ...)
{
    wchar_t buf[1024];

    if (!g_out) return;
    {
        va_list ap;
        va_start(ap, fmt);
        vswprintf(buf, fmt, ap);
        va_end(ap);
    }
    W(buf);
    fputc('\n', g_out);
    fflush(g_out);
}

static void MoveAbs(int x, int y)
{
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)((double)x * 65535.0 / (GetSystemMetrics(SM_CXSCREEN) - 1));
    in.mi.dy = (LONG)((double)y * 65535.0 / (GetSystemMetrics(SM_CYSCREEN) - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &in, sizeof(INPUT));
    Sleep(15);
}

static void Btn(DWORD f)
{
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = f;
    SendInput(1, &in, sizeof(INPUT));
    Sleep(15);
}

static void SetClip(const wchar_t *s)
{
    int n = (int)wcslen(s);
    HGLOBAL h;
    wchar_t *p;
    int tries;

    for (tries = 0; tries < 10; tries++) {
        if (OpenClipboard(NULL)) break;
        Sleep(20);
    }
    if (tries == 10) return;
    EmptyClipboard();
    h = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(wchar_t));
    if (h) {
        p = (wchar_t *)GlobalLock(h);
        if (p) {
            wmemcpy(p, s, n + 1);
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
    int tries;

    buf[0] = 0;
    for (tries = 0; tries < 10; tries++) {
        if (OpenClipboard(NULL)) break;
        Sleep(20);
    }
    if (tries == 10) return;
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
    int i;

    g_out = _wfopen(L"e2e_out.txt", L"w");
    if (!g_out) return 1;

    for (i = 0; i < 80; i++) {
        hWin = FindWindowW(L"AutoClipTestTarget", NULL);
        if (hWin) break;
        Sleep(100);
    }
    if (!hWin) { Say(L"FAIL target window not found"); fclose(g_out); return 1; }

    hEdit = FindWindowExW(hWin, NULL, L"EDIT", NULL);
    if (!hEdit) { Say(L"FAIL edit control not found"); fclose(g_out); return 1; }

    SetForegroundWindow(hWin);
    Sleep(500);
    GetWindowRect(hEdit, &r);
    Say(L"edit rect = %ld,%ld  %ld,%ld", r.left, r.top, r.right, r.bottom);

    /* ---------------- 1) drag select ---------------- */
    SetClip(L"MARKER_0000");
    Sleep(150);
    MoveAbs(r.left + 40, r.top + 13);
    Sleep(120);
    Btn(MOUSEEVENTF_LEFTDOWN);
    MoveAbs(r.left + 70, r.top + 13);
    MoveAbs(r.left + 110, r.top + 14);
    MoveAbs(r.left + 150, r.top + 14);
    Btn(MOUSEEVENTF_LEFTUP);
    Sleep(1000);

    GetClip(clip, 512);
    Say(L"copy   : clipboard=\"%ls\"", clip);
    if (wcsstr(clip, L"ALPHA") != NULL && wcsstr(clip, L"MARKER_0000") == NULL)
        Say(L"copy   : PASS");
    else { Say(L"copy   : FAIL"); g_fail = 1; }

    /* ---------------- 2) double click + paste ---------------- */
    SetClip(L"ZZZPASTE");
    Sleep(150);
    SendMessageW(hEdit, WM_SETTEXT, 0, (LPARAM)L"ALPHA BETA GAMMA\r\n\r\n\r\n");
    Sleep(150);
    MoveAbs(r.left + 140, r.top + 34);
    Sleep(120);
    Btn(MOUSEEVENTF_LEFTDOWN);
    Btn(MOUSEEVENTF_LEFTUP);
    Sleep(60);
    Btn(MOUSEEVENTF_LEFTDOWN);
    Btn(MOUSEEVENTF_LEFTUP);
    Sleep(1200);

    SendMessageW(hEdit, WM_GETTEXT, 512, (LPARAM)content);
    Say(L"paste  : edit=\"%ls\"", content);
    if (wcsstr(content, L"ZZZPASTE") != NULL) Say(L"paste  : PASS");
    else { Say(L"paste  : FAIL"); g_fail = 1; }

    /* ---------------- 3) double click on a word must not paste ---------------- */
    SetClip(L"QQQCLOBBER");
    Sleep(150);
    SendMessageW(hEdit, WM_SETTEXT, 0, (LPARAM)L"ALPHA BETA GAMMA\r\n\r\n\r\n");
    Sleep(150);
    MoveAbs(r.left + 20, r.top + 13);
    Sleep(120);
    Btn(MOUSEEVENTF_LEFTDOWN);
    Btn(MOUSEEVENTF_LEFTUP);
    Sleep(60);
    Btn(MOUSEEVENTF_LEFTDOWN);
    Btn(MOUSEEVENTF_LEFTUP);
    Sleep(1200);

    SendMessageW(hEdit, WM_GETTEXT, 512, (LPARAM)content);
    GetClip(clip, 512);
    Say(L"word   : edit=\"%ls\" clipboard=\"%ls\"", content, clip);
    if (wcsstr(content, L"QQQCLOBBER") == NULL && wcsstr(content, L"ALPHA") != NULL)
        Say(L"word   : PASS (word selected, clipboard kept)");
    else { Say(L"word   : FAIL"); g_fail = 1; }

    Say(L"RESULT : %ls", g_fail ? L"FAIL" : L"ALL PASS");
    PostMessageW(hWin, WM_CLOSE, 0, 0);
    fclose(g_out);
    return g_fail;
}
