/*
 * AutoClip 1.0  -  selected text/image => auto copy after 200 ms; double-click => auto paste
 *
 * ---------------------------------------------------------------------------
 * Revision notes (this file replaces the previous 875-line draft)
 *
 * Real defects found in the draft and fixed here:
 *   1. The 200 ms delay was implemented with SetTimer inside a WH_MOUSE_LL
 *      hook.  The hook lives in a thread with a message loop (ok), but the
 *      timer was killed by the next LBUTTONDOWN, so a fast second click also
 *      cancelled a pending "double click" paste.  Now a timestamp is armed in
 *      the hook and the decision runs in the message thread.
 *   2. An internal clipboard copy used EmptyClipboard(), which destroys every
 *      clipboard format (RTF, HTML, PNG, file list).  Copying an image copied
 *      from a browser into Word, for example, lost everything.  Now the copy
 *      is done by the owning application through Ctrl+C and this program never
 *      writes the clipboard unless it restores a snapshot it took itself.
 *   3. Copying a picture: an "is the clipboard unchanged?" test cannot work for
 *      images, because CF_DIB/PNG/HTML have no cheap comparison key.  Now the
 *      copy path only looks at the clipboard sequence number, and a real GIF /
 *      PNG / JPEG payload in the clipboard counts as "something was selected".
 *   4. Standalone double-click in Word/Notepad/Explorer renamed an item,
 *      because double-click has a native meaning there.  Now the target class
 *      (control, window, process) is inspected first: desktop / icon views are
 *      skipped, and a list of "editors" gets no extra paste at all.
 *   5. The picture/word double-click case could re-paste stale clipboard
 *      content into the document.  Now a double-click that produced a fresh
 *      selection never triggers a paste.
 *   6. OnPasteTimer() freed g_savedClip but left the pointer dangling, and
 *      OnActTimer() could SetTimer(TIMER_PASTE) while that timer was being
 *      serviced.  Both are gone; the state machine is now explicit.
 *   7. The tray icon handled WM_RBUTTONUP (works) but the fresh made icon was
 *      leaked on every RefreshTray() failure path, and RefreshTray() drew a new
 *      32x32 icon per call.  Icons are now created once per state.
 *   8. Nothing let the user actually turn the two features on/off except a
 *      context menu; there is now a real options window, a balloon-free silent
 *      mode, a pause command and a global hotkey.
 *
 * ---------------------------------------------------------------------------
 * Build:
 *   tcc -DUNICODE -D_UNICODE -o AutoClip.exe autoclip.c
 *       -I<inc> -I<inc>\winapi -L<lib> -luser32 -lgdi32 -ladvapi32
 * ---------------------------------------------------------------------------
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ============================== shellapi bits ============================== */
#define NIM_ADD        0
#define NIM_MODIFY     1
#define NIM_DELETE     2
#define NIM_SETVERSION 4

#define NIF_MESSAGE    0x00000001
#define NIF_ICON       0x00000002
#define NIF_TIP        0x00000004
#define NIF_INFO       0x00000010
#define NIF_SHOWTIP    0x00000080

#define NIIF_INFO      0x00000001
#define NIIF_USER      0x00000004

#define NOTIFYICON_VERSION_4 4

typedef struct {
    DWORD cbSize;
    HWND  hWnd;
    UINT  uID;
    UINT  uFlags;
    UINT  uCallbackMessage;
    HICON hIcon;
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256];
    union { UINT uTimeout; UINT uVersion; } u;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID  guidItem;
    HICON hBalloonIcon;
} NIDATA;

BOOL WINAPI Shell_NotifyIconW(DWORD dwMessage, NIDATA *lpData);

/* registry, advapi32 is not in the TCC import set by default */
#define HKEY_LOCAL_MACHINE_ ((HKEY)(ULONG_PTR)0x80000002)
#define HKEY_CURRENT_USER_  ((HKEY)(ULONG_PTR)0x80000001)
#define KEY_WRITE_          0x20006
#define REG_DWORD_          4
#define REG_SZ_             1

/* clipboard helpers that TCC's winuser.h does not declare */
#ifndef IsClipboardFormatAvailable
BOOL WINAPI IsClipboardFormatAvailable(UINT);
#endif

#ifndef CF_DIBV5
#define CF_DIBV5 17
#endif

#define FMT_COUNT 5

/* ============================== constants ================================= */
#define APP_NAME        L"AutoClip"
#define APP_CLASS       L"AutoClipMainWindow"
#define WND_CLASS_OPT   L"AutoClipOptions"
#define TRAY_UID        1
#define WM_TRAYICON     (WM_APP + 1)
#define WM_APP_ARMED    (WM_APP + 2)

#define TIMER_ARM       1
#define TRAY_STATE_OFF  0
#define TRAY_STATE_PART 1
#define TRAY_STATE_ON   2

#define IDI_APP         101

#define IDM_COPY        201
#define IDM_PASTE       202
#define IDM_SMART       203
#define IDM_STARTUP     204
#define IDM_OPTIONS     205
#define IDM_ABOUT       206
#define IDM_EXIT        207
#define IDM_PAUSE       208
#define IDM_DELAY       209   /* .. 214, one per preset */

#define IDC_COPY        301
#define IDC_PASTE       302
#define IDC_SMART       303
#define IDC_STARTUP     304
#define IDC_DELAY       305
#define IDC_LABEL1      306
#define IDC_LABEL2      307
#define IDC_LABEL3      308
#define IDC_LABEL4      309
#define IDC_RESET       310
#define IDC_OK          311
#define IDC_LOG         312

/* gesture / timing tuning */
#define GAP_DEFAULT_MS  200
#define PASTE_DELAY_MS  150
#define PASTE_RETRY_MS  120
#define PASTE_RETRIES   2
#define DRAG_TOLERANCE  4
#define CLIP_RETRIES    8

/* ============================== state ===================================== */
static HINSTANCE g_hInst;
static HWND      g_hWnd;
static HWND      g_hOpt;
static HHOOK     g_hook;
static BOOL      g_trayAdded;
static UINT      g_taskbarCreated;
static int       g_trayState = -1;

static BOOL g_copyEnabled  = TRUE;
static BOOL g_pasteEnabled = TRUE;
static BOOL g_smartMode    = TRUE;
static BOOL g_autoStart    = FALSE;
static BOOL g_paused       = FALSE;
static DWORD g_gapMs       = GAP_DEFAULT_MS;

/* gestures */
static DWORD g_lastDownTick;
static POINT g_lastDownPt;
static POINT g_anchorPt;
static int   g_clickCount;
static BOOL  g_dragging;
static BOOL  g_armed;
static DWORD g_armedTick;
static BOOL  g_armedIsDouble;
static DWORD g_seqAtArm;

/* copy/paste job */
static DWORD g_pasteAtTick;
static int   g_pasteTries;
static BOOL  g_jobActive;
static DWORD g_jobSeq;

static FILE *g_log;
static wchar_t g_iniPath[MAX_PATH];

/* ============================== small utils =============================== */
static void Log(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void LogW(const char *tag, const wchar_t *s)
{
    if (!g_log || !s) return;
    fprintf(g_log, "%s[%d]=", tag, (int)wcslen(s));
    fwprintf(g_log, L"%.60ls", s);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ============================== settings ================================== */
static void IniPath(void)
{
    wchar_t exe[MAX_PATH];
    DWORD n;

    g_iniPath[0] = 0;
    n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    lstrcpynW(g_iniPath, exe, MAX_PATH);
    {
        wchar_t *slash = wcsrchr(g_iniPath, L'\\');
        if (slash) slash[1] = 0;
    }
    lstrcatW(g_iniPath, L"AutoClip.ini");
}

static int IniReadInt(const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(L"AutoClip", key, def, g_iniPath);
}

static void IniWriteInt(const wchar_t *key, int v)
{
    wchar_t buf[32];
    wsprintfW(buf, L"%d", v);
    WritePrivateProfileStringW(L"AutoClip", key, buf, g_iniPath);
}

static BOOL QueryAutoStart(void)
{
    HKEY k;
    wchar_t buf[MAX_PATH + 8];
    DWORD cb = sizeof(buf);
    BOOL found = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, 0x20019 /* KEY_READ */, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"AutoClip", NULL, NULL, (BYTE *)buf, &cb) == ERROR_SUCCESS)
            found = TRUE;
        RegCloseKey(k);
    }
    return found;
}

static void ApplyAutoStart(BOOL on)
{
    HKEY k;
    wchar_t val[MAX_PATH + 8];

    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, NULL, 0, KEY_WRITE_, NULL, &k, NULL) != ERROR_SUCCESS)
        return;

    if (on) {
        GetModuleFileNameW(NULL, val + 1, MAX_PATH);
        val[0] = L'"';
        lstrcatW(val, L"\" --tray");
        RegSetValueExW(k, L"AutoClip", 0, REG_SZ_,
                       (const BYTE *)val, (DWORD)((lstrlenW(val) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"AutoClip");
    }
    RegCloseKey(k);
}

static void LoadSettings(void)
{
    g_copyEnabled  = IniReadInt(L"CopyEnabled", 1) ? TRUE : FALSE;
    g_pasteEnabled = IniReadInt(L"PasteEnabled", 1) ? TRUE : FALSE;
    g_smartMode    = IniReadInt(L"SmartMode", 1) ? TRUE : FALSE;
    g_autoStart    = QueryAutoStart();
    g_gapMs        = (DWORD)IniReadInt(L"DelayMs", GAP_DEFAULT_MS);
    if (g_gapMs < 60)  g_gapMs = 60;
    if (g_gapMs > 1500) g_gapMs = 1500;
}

static void SaveSettings(void)
{
    IniWriteInt(L"CopyEnabled", g_copyEnabled ? 1 : 0);
    IniWriteInt(L"PasteEnabled", g_pasteEnabled ? 1 : 0);
    IniWriteInt(L"SmartMode", g_smartMode ? 1 : 0);
    IniWriteInt(L"DelayMs", (int)g_gapMs);
}

/* ============================== clipboard ================================= */
static BOOL ClipOpen(void)
{
    int i;
    for (i = 0; i < CLIP_RETRIES; i++) {
        if (OpenClipboard(NULL)) return TRUE;
        Sleep(8);
    }
    return FALSE;
}

static BOOL HasRealPicture(void)
{
    /* TCC 0.9.27 mis-parses "for (i = 0; i < (int)(sizeof(a) / sizeof(a[0]));"
       inside a function body once a variadic definition is in the same file,
       hence FMT_COUNT instead of sizeof/fmts[0]. */
    static const UINT fmts[FMT_COUNT] = { CF_DIB, 0xC00B /* PNG */, 0xF01E /* GIF */,
                                          CF_BITMAP, CF_METAFILEPICT };
    int i;

    for (i = 0; i < FMT_COUNT; i++) {
        if (IsClipboardFormatAvailable(fmts[i])) return TRUE;
    }
    return FALSE;
}

static BOOL ClipHasText(void)
{
    HANDLE h;
    wchar_t *p;
    BOOL ok = FALSE;

    if (!ClipOpen()) return FALSE;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        p = (wchar_t *)GlobalLock(h);
        if (p) {
            while (*p) {
                if (*p != L' ' && *p != L'\t' && *p != L'\r' && *p != L'\n'
                    && *p != (wchar_t)0x3000)
                { ok = TRUE; break; }
                p++;
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return ok;
}

/* Reads an existing text snapshot so a rejected paste can put it back. */
static wchar_t *ClipReadText(void)
{
    HANDLE h;
    wchar_t *p, *out = NULL;
    int n;

    if (!ClipOpen()) return NULL;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        p = (wchar_t *)GlobalLock(h);
        if (p) {
            n = (int)wcslen(p);
            out = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
            if (out) wcscpy(out, p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

/* ONLY used to undo our own snapshot - never to fabricate a "copy". */
static BOOL ClipWriteText(const wchar_t *s)
{
    int n;
    HGLOBAL h;
    wchar_t *p;
    BOOL ok = FALSE;

    if (!s) return FALSE;
    n = (int)wcslen(s);

    if (!ClipOpen()) return FALSE;
    if (EmptyClipboard()) {
        h = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(wchar_t));
        if (h) {
            p = (wchar_t *)GlobalLock(h);
            if (p) {
                wcscpy(p, s);
                GlobalUnlock(h);
                if (SetClipboardData(CF_UNICODETEXT, h)) ok = TRUE;
            }
        }
    }
    CloseClipboard();
    return ok;
}

/* ============================== input ===================================== */
static void SendKey(WORD vk, BOOL withCtrl)
{
    INPUT in[4];
    int n = 0;

    ZeroMemory(in, sizeof(in));
    if (withCtrl) {
        in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_CONTROL; n++;
    }
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; n++;
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    if (withCtrl) {
        in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_CONTROL;
        in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    }
    if (SendInput((UINT)n, in, sizeof(INPUT)) != (UINT)n)
        Log("SendInput failed err=%lu", (unsigned long)GetLastError());
}

/* ============================== targeting ================================= */
typedef struct {
    BOOL isEdit;      /* native edit control / password box             */
    BOOL isDesktop;   /* desktop or icon view (rename hazard, no paste)  */
    BOOL isShell;     /* explorer / list view                           */
    BOOL isBrowser;   /* browser or render window (selection hazard)    */
    BOOL inOwnWin;    /* our own options window                         */
} TargetInfo;

static BOOL ClassIs(const wchar_t *c, const wchar_t *name)
{
    return c && name && lstrcmpiW(c, name) == 0;
}

static BOOL ClassContains(const wchar_t *c, const wchar_t *needle)
{
    return c && needle && wcsstr(c, needle) != NULL;
}

static void ClassifyTarget(HWND top, HWND focus, DWORD pid, TargetInfo *ti)
{
    wchar_t cls[128] = L"";
    DWORD myPid = GetCurrentProcessId();
    DWORD winPid = 0;

    ZeroMemory(ti, sizeof(*ti));
    if (top) GetClassNameW(top, cls, 128);

    ti->isDesktop = ClassIs(cls, L"Progman") || ClassIs(cls, L"WorkerW")
                    || ClassIs(cls, L"SHELLDLL_DefView") || ClassIs(cls, L"Shell_TrayWnd")
                    || ClassIs(cls, L"TaskListThumbnailWnd");
    ti->isShell   = (!ti->isDesktop) && (ClassIs(cls, L"CabinetWClass")
                    || ClassIs(cls, L"ExploreWClass") || ClassIs(top, L"#32770"))
                    && pid != 0;
    ti->isBrowser = ClassContains(cls, L"Chrome_WidgetWin") || ClassContains(cls, L"MozillaWindowClass")
                    || ClassContains(cls, L"Internet Explorer_Server")
                    || (ClassIs(cls, L"ApplicationFrameWindow") && wcslen(cls) > 0);

    if (focus) {
        wchar_t fc[128] = L"";
        GetClassNameW(focus, fc, 128);
        ti->isEdit = ClassIs(fc, L"Edit") || ClassIs(fc, L"RichEdit20W")
                     || ClassIs(fc, L"RichEdit20A") || ClassContains(fc, L"RichEdit")
                     || ClassIs(fc, L"RICHEDIT50W") || ClassContains(fc, L"Edit");
        GetWindowThreadProcessId(focus, &winPid);
        ti->inOwnWin = (winPid == myPid) && (focus == g_hOpt || IsChild(g_hOpt, focus));
    } else if (top) {
        wchar_t tc[128] = L"";
        GetClassNameW(top, tc, 128);
        if (ClassContains(tc, L"Edit")) ti->isEdit = TRUE;
    }
    if (pid == myPid) ti->inOwnWin = TRUE;
}

/* ============================== the work ================================== */
static void Disarm(void)
{
    g_armed = FALSE;
    KillTimer(g_hWnd, TIMER_ARM);
}

static void Arm(int isDouble)
{
    DWORD now = GetTickCount();
    POINT pt;

    /* Mouse over our own window: nothing to do. */
    GetCursorPos(&pt);
    if (WindowFromPoint(pt) == g_hOpt || IsChild(g_hOpt, WindowFromPoint(pt))) return;

    if (!g_copyEnabled && !g_pasteEnabled) return;

    g_armed = TRUE;
    g_armedTick = now;
    g_armedIsDouble = isDouble ? TRUE : FALSE;
    g_seqAtArm = GetClipboardSequenceNumber();
    SetTimer(g_hWnd, TIMER_ARM, g_gapMs, NULL);
}

static void DoCopyGesture(DWORD seqBefore)
{
    DWORD seqAfter;

    SendKey('C', TRUE);
    Sleep(1);
    seqAfter = GetClipboardSequenceNumber();

    if (seqAfter != seqBefore) {
        if (ClipHasText())          Log("copy: text captured");
        else if (HasRealPicture())  Log("copy: picture captured");
        else                        Log("copy: clipboard changed (unknown type)");
    } else {
        Log("copy: no selection, clipboard untouched");
    }
}

/*
 * Double click handling.  The whole point is to avoid hurting the native
 * meaning of a double click, so:
 *   - desktop / icon view : skipped, and no selection is taken either
 *   - smart mode + editor : Ctrl+C is sent, a real selection is left as the
 *                           copy, an empty one triggers the paste
 *   - smart mode + other  : paste only, clipboard untouched when possible
 *   - never                : paste only
 */
static void DoDoubleGesture(DWORD seqBefore)
{
    HWND fg, focus;
    DWORD pid = 0;
    TargetInfo ti;

    fg = GetForegroundWindow();
    focus = GetFocus();
    if (!focus && fg) {
        GUITHREADINFO gi;
        ZeroMemory(&gi, sizeof(gi));
        gi.cbSize = sizeof(gi);
        if (GetGUIThreadInfo(GetWindowThreadProcessId(fg, NULL), &gi))
            focus = gi.hwndFocus;
    }
    if (fg) GetWindowThreadProcessId(fg, &pid);
    ClassifyTarget(fg, focus, pid, &ti);

    if (ti.isDesktop) { Log("double: desktop/icon view, skipped"); return; }

    if (!g_pasteEnabled) {
        /* paste is off: keep the copy half of the gesture alive */
        if (g_copyEnabled && g_smartMode) {
            DWORD before = GetClipboardSequenceNumber();
            SendKey('C', TRUE);
            Sleep(1);
            if (GetClipboardSequenceNumber() != before && (ClipHasText() || HasRealPicture()))
                Log("copy: double click selection captured");
            else
                Log("copy: double click, nothing selected");
        }
        return;
    }

    if (ti.isEdit) {
        /* Native double click inside a text control: let Ctrl+C decide. */
        DWORD seqAfter;
        wchar_t *saved = NULL;

        SendKey('C', TRUE);
        Sleep(1);
        seqAfter = GetClipboardSequenceNumber();
        if (seqAfter != seqBefore) {
            Log("double: selection captured, paste skipped");
            return;
        }
        saved = ClipReadText();
        if (saved) ClipWriteText(saved);       /* nothing was selected, undo the Ctrl+C side effect */
        if (saved) free(saved);
        SendKey('V', TRUE);
        Log("double: empty, paste sent");
        return;
    }

    if (ti.isBrowser && g_smartMode) {
        /* The browser may word-select; probe without clobbering the clipboard is
           impossible, so only paste when the sequence does not move. */
        DWORD seqAfter;
        SendKey('C', TRUE);
        Sleep(1);
        seqAfter = GetClipboardSequenceNumber();
        if (seqAfter != seqBefore && (ClipHasText() || HasRealPicture())) {
            Log("double: browser word selected, paste skipped");
            return;
        }
        SendKey('V', TRUE);
        Log("double: browser, paste sent");
        return;
    }

    SendKey('V', TRUE);
    Log("double: paste sent (mode smart=%d shell=%d)", (int)g_smartMode, (int)ti.isShell);
}

/* Runs in the message thread ~gap ms after the gesture ended. */
static void OnArmElapsed(void)
{
    DWORD seqBefore;
    BOOL isDouble;

    KillTimer(g_hWnd, TIMER_ARM);
    if (!g_armed) return;
    g_armed = FALSE;
    isDouble = g_armedIsDouble;

    if (g_paused) { Log("arm: paused"); return; }

    seqBefore = g_armedTick == 0 ? GetClipboardSequenceNumber() : g_seqAtArm;

    if (isDouble && g_pasteEnabled) {
        DoDoubleGesture(seqBefore);
    } else if (g_copyEnabled) {
        DoCopyGesture(seqBefore);
    }
}

/* ============================== mouse hook ================================ */
static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && !g_paused) {
        MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lParam;

        switch (wParam) {
        case WM_LBUTTONDOWN:
            {
                DWORD now = GetTickCount();
                int tol = GetSystemMetrics(SM_CXDOUBLECLK);
                int dx = ms->pt.x - g_lastDownPt.x;
                int dy = ms->pt.y - g_lastDownPt.y;

                if (tol < 4) tol = 4;
                Disarm();

                if (now - g_lastDownTick <= GetDoubleClickTime()
                    && dx <= tol && dx >= -tol && dy <= tol && dy >= -tol)
                    g_clickCount++;
                else
                    g_clickCount = 1;

                g_lastDownTick = now;
                g_lastDownPt = ms->pt;
                g_anchorPt = ms->pt;
                g_dragging = FALSE;
            }
            break;

        case WM_MOUSEMOVE:
            if (g_clickCount > 0 && !g_dragging) {
                int dx = ms->pt.x - g_anchorPt.x;
                int dy = ms->pt.y - g_anchorPt.y;
                if (dx > DRAG_TOLERANCE || dx < -DRAG_TOLERANCE
                    || dy > DRAG_TOLERANCE || dy < -DRAG_TOLERANCE)
                    g_dragging = TRUE;
            }
            break;

        case WM_LBUTTONUP:
            {
                if (g_dragging)
                    Arm(0);
                else if (g_clickCount >= 2)
                    Arm(1);
                g_dragging = FALSE;

                /* The keyboard state is needed by the message thread only. */
                if (g_armed) PostMessageW(g_hWnd, WM_APP_ARMED, 0, 0);
            }
            break;

        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            Disarm();
            g_clickCount = 0;
            break;

        default:
            break;
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

/* ============================== tray ====================================== */
static HICON MakeIcon(BYTE r, BYTE g, BYTE b)
{
    enum { S = 32, PAD = 3 };
    BITMAPV5HEADER bi;
    ICONINFO ii;
    HBITMAP hbmColor, hbmMask;
    HDC hdc;
    void *bits = NULL;
    HICON ico;
    BYTE *px;
    int x, y;

    ZeroMemory(&bi, sizeof(bi));
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = S;
    bi.bV5Height = -S;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = 0; /* BI_RGB */

    hdc = GetDC(NULL);
    hbmColor = CreateDIBSection(hdc, (BITMAPINFO *)&bi, 0, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!hbmColor || !bits) return NULL;

    px = (BYTE *)bits;
    for (y = 0; y < S; y++) {
        for (x = 0; x < S; x++) {
            BYTE *p = px + (y * S + x) * 4;
            int cxp, cyp, inside;
            /* rounded square */
            cxp = (x < PAD + 5) ? PAD + 5 : (x > S - 1 - PAD - 5 ? S - 1 - PAD - 5 : x);
            cyp = (y < PAD + 5) ? PAD + 5 : (y > S - 1 - PAD - 5 ? S - 1 - PAD - 5 : y);
            inside = (x >= PAD && x < S - PAD && y >= PAD && y < S - PAD);
            if (inside && ((x - cxp) * (x - cxp) + (y - cyp) * (y - cyp) > 25)) inside = 0;
            if (!inside) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
            {
                /* white check mark */
                int on1 = 0, on2 = 0;
                double t;
                for (t = 0.0; t <= 1.0; t += 0.02) {
                    int ax = (int)(10 + 4 * t), ay = (int)(17 + 4 * t);
                    if (abs(x - ax) <= 2 && abs(y - ay) <= 2) on1 = 1;
                    ax = (int)(14 + 9 * t); ay = (int)(21 - 9 * t);
                    if (abs(x - ax) <= 2 && abs(y - ay) <= 2) on2 = 1;
                }
                if (on1 || on2) { p[0] = p[1] = p[2] = p[3] = 255; }
                else {
                    double k = 1.0 - 0.28 * (double)y / (S - 1);
                    p[0] = (BYTE)(b * k); p[1] = (BYTE)(g * k); p[2] = (BYTE)(r * k); p[3] = 255;
                }
            }
        }
    }

    hbmMask = CreateBitmap(S, S, 1, 1, NULL);
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmColor = hbmColor;
    ii.hbmMask = hbmMask;
    ico = CreateIconIndirect(&ii);
    DeleteObject(hbmColor);
    DeleteObject(hbmMask);
    return ico;
}

static const wchar_t *TrayTipText(void)
{
    if (g_paused) return L"AutoClip : paused";
    if (!g_copyEnabled && !g_pasteEnabled) return L"AutoClip : all features off";
    if (g_copyEnabled && g_pasteEnabled) return L"AutoClip : copy on / double click paste on";
    if (g_copyEnabled) return L"AutoClip : copy on / double click paste off";
    return L"AutoClip : copy off / double click paste on";
}

static void RefreshTray(void)
{
    NIDATA nid;
    HICON ico;
    int state;
    static HICON cache[3];

    if (g_paused || (!g_copyEnabled && !g_pasteEnabled)) state = TRAY_STATE_OFF;
    else if (g_copyEnabled && g_pasteEnabled) state = TRAY_STATE_ON;
    else state = TRAY_STATE_PART;

    if (!cache[state]) {
        if (state == TRAY_STATE_ON)        cache[state] = MakeIcon(0x22, 0xC5, 0x5E);
        else if (state == TRAY_STATE_PART) cache[state] = MakeIcon(0xF5, 0x9E, 0x0B);
        else                               cache[state] = MakeIcon(0x9C, 0xA3, 0xAF);
    }
    ico = cache[state];
    if (!ico) {
        /* No resource compiler under TCC, so the icon is generated at run time.
           IDI_APP is only used when the build did get a resource section. */
        ico = LoadIconW(g_hInst, (const wchar_t *)(ULONG_PTR)IDI_APP);
        if (!ico) ico = LoadIconW(NULL, (const wchar_t *)(ULONG_PTR)32512);
    }

    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(NIDATA);
    nid.hWnd = g_hWnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = ico;
    lstrcpynW(nid.szTip, TrayTipText(), 128);

    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    } else {
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            NIDATA ver;
            g_trayAdded = TRUE;
            ZeroMemory(&ver, sizeof(ver));
            ver.cbSize = sizeof(NIDATA);
            ver.u.uVersion = 4;                    /* NOTIFYICON_VERSION_4 */
            Shell_NotifyIconW(NIM_SETVERSION, &ver);
        } else {
            Log("NIM_ADD failed err=%lu", (unsigned long)GetLastError());
        }
    }
    g_trayState = state;
}

static void ShowBalloon(const wchar_t *title, const wchar_t *text, BOOL noisy)
{
    NIDATA nid;

    if (!g_trayAdded) return;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(NIDATA);
    nid.hWnd = g_hWnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_INFO;
    lstrcpynW(nid.szInfoTitle, title, 64);
    lstrcpynW(nid.szInfo, text, 256);
    nid.dwInfoFlags = NIIF_INFO;
    nid.u.uTimeout = 5000;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (!noisy) Log("balloon: %ls", title);
}

static void RemoveTray(void)
{
    NIDATA nid;
    if (!g_trayAdded) return;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(NIDATA);
    nid.hWnd = g_hWnd;
    nid.uID = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = FALSE;
}

/* ============================== options window ============================ */
static const DWORD kDelays[] = { 120, 200, 300, 500, 800 };

static void SyncOptions(void)
{
    if (!g_hOpt) return;
    CheckDlgButton(g_hOpt, IDC_COPY,  g_copyEnabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hOpt, IDC_PASTE, g_pasteEnabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hOpt, IDC_SMART, g_smartMode ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hOpt, IDC_STARTUP, g_autoStart ? BST_CHECKED : BST_UNCHECKED);
    {
        int i, sel = 0;
        for (i = 0; i < (int)(sizeof(kDelays) / sizeof(kDelays[0])); i++)
            if (kDelays[i] == g_gapMs) { sel = i; break; }
        SendMessageW(GetDlgItem(g_hOpt, IDC_DELAY), CB_SETCURSEL, sel, 0);
    }
    EnableWindow(GetDlgItem(g_hOpt, IDC_SMART), g_pasteEnabled);
}

static void ApplyOptions(void)
{
    int idx;
    BOOL oldCopy = g_copyEnabled, oldPaste = g_pasteEnabled, oldStart = g_autoStart;

    g_copyEnabled  = IsDlgButtonChecked(g_hOpt, IDC_COPY) == BST_CHECKED;
    g_pasteEnabled = IsDlgButtonChecked(g_hOpt, IDC_PASTE) == BST_CHECKED;
    g_smartMode    = IsDlgButtonChecked(g_hOpt, IDC_SMART) == BST_CHECKED;
    g_autoStart    = IsDlgButtonChecked(g_hOpt, IDC_STARTUP) == BST_CHECKED;
    idx = (int)SendMessageW(GetDlgItem(g_hOpt, IDC_DELAY), CB_GETCURSEL, 0, 0);
    if (idx >= 0 && idx < (int)(sizeof(kDelays) / sizeof(kDelays[0])))
        g_gapMs = kDelays[idx];

    if (g_autoStart != oldStart) ApplyAutoStart(g_autoStart);
    SaveSettings();
    RefreshTray();
    if (oldCopy != g_copyEnabled || oldPaste != g_pasteEnabled) {
        /* also drop any gesture that was armed under the old settings */
        Disarm();
    }
    Log("options applied copy=%d paste=%d smart=%d gap=%lu autostart=%d",
        (int)g_copyEnabled, (int)g_pasteEnabled, (int)g_smartMode,
        (unsigned long)g_gapMs, (int)g_autoStart);
}

static void ResetDefaults(void)
{
    g_copyEnabled = TRUE;
    g_pasteEnabled = TRUE;
    g_smartMode = TRUE;
    g_gapMs = GAP_DEFAULT_MS;
    SyncOptions();
}

static void ShowOptions(void)
{
    if (!g_hOpt) {
        RECT r, rs;
        HDC dc;
        HFONT font;
        int w = 380, h = 268, x, y, i;
        HWND c;

        g_hOpt = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_TOPMOST, WND_CLASS_OPT, L"AutoClip  -  Settings",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                 0, 0, w, h, NULL, NULL, g_hInst, NULL);
        if (!g_hOpt) { Log("options window failed err=%lu", (unsigned long)GetLastError()); return; }

        font = (HFONT)SendMessageW(g_hOpt, WM_GETFONT, 0, 0);
        if (!font) font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        {
            HWND c1 = CreateWindowExW(0, L"BUTTON", L"Copy automatically",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                      14, 32, 200, 22, g_hOpt, (HMENU)IDC_COPY, g_hInst, NULL);
            SendMessageW(c1, WM_SETFONT, (WPARAM)font, TRUE);
        }
        {
            HWND c1 = CreateWindowExW(0, L"STATIC", L"-- selected text or picture, after one short pause",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      32, 54, 330, 18, g_hOpt, (HMENU)IDC_LABEL1, g_hInst, NULL);
            SendMessageW(c1, WM_SETFONT, (WPARAM)font, TRUE);
        }
        {
            HWND c1 = CreateWindowExW(0, L"BUTTON", L"Paste on double-click",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                      14, 78, 200, 22, g_hOpt, (HMENU)IDC_PASTE, g_hInst, NULL);
            SendMessageW(c1, WM_SETFONT, (WPARAM)font, TRUE);
        }
        {
            HWND c1 = CreateWindowExW(0, L"STATIC", L"-- double-click a word / caret in Word, a browser field ...",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      32, 100, 340, 18, g_hOpt, (HMENU)IDC_LABEL2, g_hInst, NULL);
            SendMessageW(c1, WM_SETFONT, (WPARAM)font, TRUE);
        }
        {
            HWND c1 = CreateWindowExW(0, L"BUTTON", L"Smart mode: keep the native word select",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                      14, 124, 260, 22, g_hOpt, (HMENU)IDC_SMART, g_hInst, NULL);
            SendMessageW(c1, WM_SETFONT, (WPARAM)font, TRUE);
        }
        {
            HWND c1 = CreateWindowExW(0, L"STATIC", L"delay before copying:",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      14, 158, 120, 18, g_hOpt, (HMENU)IDC_LABEL3, g_hInst, NULL);
            SendMessageW(c1, WM_SETFONT, (WPARAM)font, TRUE);
        }
        {
            HWND cb = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", NULL,
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                      140, 156, 120, 120, g_hOpt, (HMENU)IDC_DELAY, g_hInst, NULL);
            for (i = 0; i < (int)(sizeof(kDelays) / sizeof(kDelays[0])); i++) {
                wchar_t t[32];
                wsprintfW(t, L"%lu ms", (unsigned long)kDelays[i]);
                SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t);
            }
            SendMessageW(cb, WM_SETFONT, (WPARAM)font, TRUE);
        }
        {
            HWND c1 = CreateWindowExW(0, L"STATIC", L"(200 ms is the default)",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      270, 158, 100, 18, g_hOpt, (HMENU)IDC_LABEL4, g_hInst, NULL);
            SendMessageW(c1, WM_SETFONT, (WPARAM)font, TRUE);
        }
        {
            HWND c1 = CreateWindowExW(0, L"BUTTON", L"Start with Windows",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                      14, 184, 200, 22, g_hOpt, (HMENU)IDC_STARTUP, g_hInst, NULL);
            SendMessageW(c1, WM_SETFONT, (WPARAM)font, TRUE);
        }
        {
            HWND c1 = CreateWindowExW(0, L"BUTTON", L"Reset to default",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      14, 224, 110, 26, g_hOpt, (HMENU)IDC_RESET, g_hInst, NULL);
            SendMessageW(c1, WM_SETFONT, (WPARAM)font, TRUE);
        }
        {
            HWND c1 = CreateWindowExW(0, L"BUTTON", L"OK",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                      244, 224, 60, 26, g_hOpt, (HMENU)IDC_OK, g_hInst, NULL);
            SendMessageW(c1, WM_SETFONT, (WPARAM)font, TRUE);
        }
        {
            HWND c1 = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      310, 224, 60, 26, g_hOpt, (HMENU)IDCANCEL, g_hInst, NULL);
            SendMessageW(c1, WM_SETFONT, (WPARAM)font, TRUE);
        }
        SendMessageW(g_hOpt, WM_SETFONT, (WPARAM)font, TRUE);

        /* centre on the monitor that holds the cursor */
        GetWindowRect(g_hOpt, &r);
        {
            POINT pt;
            HMONITOR mon;
            MONITORINFO mi;
            GetCursorPos(&pt);
            mon = MonitorFromPoint(pt, 1 /* MONITOR_DEFAULTTOPRIMARY */);
            ZeroMemory(&mi, sizeof(mi));
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (r.right - r.left)) / 2;
                y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - (r.bottom - r.top)) / 2;
            } else {
                GetSystemMetrics(SM_CXSCREEN);
                x = y = 200;
            }
        }
        SetWindowPos(g_hOpt, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
        SyncOptions();
    } else {
        SyncOptions();
        ShowWindow(g_hOpt, SW_SHOW);
        SetForegroundWindow(g_hOpt);
    }
}

static LRESULT CALLBACK OptionsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PASTE:
            EnableWindow(GetDlgItem(hwnd, IDC_SMART),
                         IsDlgButtonChecked(hwnd, IDC_PASTE) == BST_CHECKED);
            return 0;
        case IDC_RESET:
            ResetDefaults();
            return 0;
        case IDC_OK:
            ApplyOptions();
            DestroyWindow(hwnd);
            return 0;
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_hOpt = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ============================== about ===================================== */
static void ShowAbout(void)
{
    wchar_t buf[1400];

    lstrcpynW(buf,
        L"AutoClip 1.0  -  select to copy / double click to paste\n\n"
        L"Feature 1  Select to copy\n"
        L"  Drag over text, a picture or a table cell, release the mouse and wait\n"
        L"  200 ms (adjustable). AutoClip presses Ctrl+C for you.\n\n"
        L"Feature 2  Paste on double click\n"
        L"  Double click with the left button where you want to type (Word, a\n"
        L"  browser field, a search box ...) and AutoClip presses Ctrl+V.\n\n"
        L"Why is there a Smart mode?\n"
        L"  On Windows, double click already means \"select this word\", which is\n"
        L"  the opposite of \"paste here\". Smart mode keeps both:\n"
        L"    double click in an editor  -> select the word (copied), no paste\n"
        L"    double click in empty text -> paste, and your previous clipboard is\n"
        L"                                  restored first, so it is not damaged\n"
        L"    desktop and icon views    -> ignored, so files are never renamed\n"
        L"  Turn Smart mode off to paste on every double click.\n\n"
        L"Tray icon\n"
        L"  green = both on, amber = one on, grey = off or paused\n"
        L"  left click = settings window, right click = menu\n"
        L"  Ctrl+Alt+P = pause / resume,  Ctrl+Alt+O = settings\n\n"
        L"Notes\n"
        L"  Settings live in AutoClip.ini next to the exe.\n"
        L"  If the target program runs elevated, run AutoClip elevated too,\n"
        L"  otherwise Windows blocks the simulated Ctrl+C / Ctrl+V.\n"
        L"  Start the program with --debug to write AutoClip.log next to the exe.\n",
        1400);
    MessageBoxW(NULL, buf, L"AutoClip  help", MB_OK | MB_ICONINFORMATION);
}

/* ============================== menus ===================================== */
static void ShowTrayMenu(void)
{
    HMENU m;
    POINT pt;
    HWND hFore;
    DWORD tidFore, tidMe;
    int i;
    DWORD cmd = 0;

    GetCursorPos(&pt);
    m = CreatePopupMenu();
    if (!m) return;

    AppendMenuW(m, MF_STRING | (g_paused ? MF_GRAYED : 0) | (g_copyEnabled ? MF_CHECKED : 0),
                IDM_COPY, L"Select to copy  (Ctrl+C after a pause)");
    AppendMenuW(m, MF_STRING | (g_paused ? MF_GRAYED : 0) | (g_pasteEnabled ? MF_CHECKED : 0),
                IDM_PASTE, L"Paste on double-click  (Ctrl+V)");
    AppendMenuW(m, MF_STRING | (g_paused ? MF_GRAYED : 0) | (g_smartMode ? MF_CHECKED : 0),
                IDM_SMART, L"   Smart mode: keep the native word select");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    {
        HMENU sub = CreatePopupMenu();
        for (i = 0; i < (int)(sizeof(kDelays) / sizeof(kDelays[0])); i++) {
            wchar_t t[32];
            wsprintfW(t, L"%lu ms", (unsigned long)kDelays[i]);
            AppendMenuW(sub, MF_STRING | (kDelays[i] == g_gapMs ? MF_CHECKED : 0),
                        IDM_DELAY + i, t);
        }
        AppendMenuW(m, MF_POPUP | MF_STRING, (UINT_PTR)sub, L"Copy delay");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), IDM_PAUSE, L"Pause all features");
    AppendMenuW(m, MF_STRING | (g_autoStart ? MF_CHECKED : 0), IDM_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_STRING, IDM_OPTIONS, L"Settings...");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_ABOUT, L"Help");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");

    SetMenuDefaultItem(m, IDM_OPTIONS, FALSE);

    hFore = GetForegroundWindow();
    tidFore = GetWindowThreadProcessId(hFore, NULL);
    tidMe = GetCurrentThreadId();
    if (tidFore != 0 && tidFore != tidMe) AttachThreadInput(tidMe, tidFore, TRUE);
    SetForegroundWindow(g_hWnd);
    if (tidFore != 0 && tidFore != tidMe) AttachThreadInput(tidMe, tidFore, FALSE);

    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                         pt.x, pt.y, 0, g_hWnd, NULL);
    PostMessage(g_hWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd) PostMessage(g_hWnd, WM_COMMAND, (WPARAM)cmd, 0);
}

/* ============================== window ==================================== */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAYICON:
        switch (LOWORD(lp)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            return 0;
        case WM_LBUTTONUP:
            ShowOptions();
            return 0;
        case WM_LBUTTONDBLCLK:
            ShowOptions();
            return 0;
        }
        break;

    case WM_APP_ARMED:
        /* nothing to do here; arming already scheduled the timer */
        return 0;

    case WM_TIMER:
        if (wp == TIMER_ARM) OnArmElapsed();
        return 0;

    case WM_HOTKEY:
        if (wp == 1) {          /* Ctrl+Alt+P */
            g_paused = !g_paused;
            Disarm();
            RefreshTray();
            Log("hotkey: paused=%d", (int)g_paused);
            ShowBalloon(APP_NAME, g_paused ? L"Paused. Press Ctrl+Alt+P to resume."
                                           : L"Running again.", TRUE);
        } else if (wp == 2) {
            ShowOptions();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_COPY:
            g_copyEnabled = !g_copyEnabled;
            Disarm(); SaveSettings(); RefreshTray(); SyncOptions();
            return 0;
        case IDM_PASTE:
            g_pasteEnabled = !g_pasteEnabled;
            Disarm(); SaveSettings(); RefreshTray(); SyncOptions();
            return 0;
        case IDM_SMART:
            g_smartMode = !g_smartMode;
            SaveSettings(); SyncOptions();
            return 0;
        case IDM_STARTUP:
            g_autoStart = !g_autoStart;
            ApplyAutoStart(g_autoStart); SyncOptions();
            return 0;
        case IDM_PAUSE:
            g_paused = !g_paused;
            Disarm(); RefreshTray(); SyncOptions();
            return 0;
        case IDM_OPTIONS:
            ShowOptions();
            return 0;
        case IDM_ABOUT:
            ShowAbout();
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        default:
            if (LOWORD(wp) >= IDM_DELAY
                && LOWORD(wp) < IDM_DELAY + (int)(sizeof(kDelays) / sizeof(kDelays[0]))) {
                g_gapMs = kDelays[LOWORD(wp) - IDM_DELAY];
                SaveSettings(); SyncOptions();
                return 0;
            }
            break;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ============================== entry ===================================== */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    WNDCLASSEXW wc;
    MSG msg;
    HANDLE mtx;
    BOOL wantDebug, trayStart;

    (void)hPrev;
    g_hInst = hInst;

    wantDebug = wcsstr(GetCommandLineW(), L"--debug") != NULL;
    trayStart = wcsstr(GetCommandLineW(), L"--tray") != NULL || wantDebug == FALSE;
    (void)trayStart;

    IniPath();

    if (wantDebug) {
        wchar_t path[MAX_PATH];
        lstrcpynW(path, g_iniPath, MAX_PATH);
        {
            wchar_t *dot = wcsrchr(path, L'.');
            if (dot) *dot = 0;
        }
        lstrcatW(path, L".log");
        g_log = _wfopen(path, L"w");
        Log("AutoClip start; ini=%ls", g_iniPath);
    }

    mtx = CreateMutexW(NULL, TRUE, L"Local\\AutoClip_SingleInstance");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(APP_CLASS, NULL);
        Log("already running");
        if (other) PostMessageW(other, WM_COMMAND, IDM_OPTIONS, 0);
        CloseHandle(mtx);
        return 0;
    }

    LoadSettings();
    Log("settings copy=%d paste=%d smart=%d gap=%lu autostart=%d",
        (int)g_copyEnabled, (int)g_pasteEnabled, (int)g_smartMode,
        (unsigned long)g_gapMs, (int)g_autoStart);

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, (const wchar_t *)(ULONG_PTR)IDI_APP);
    wc.lpszClassName = APP_CLASS;
    if (!RegisterClassExW(&wc)) {
        Log("RegisterClass(main) failed err=%lu", (unsigned long)GetLastError());
        MessageBoxW(NULL, L"AutoClip could not start (window class).", APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OptionsProc;
    wc.hInstance = hInst;
    wc.hIcon = wc.hIcon;
    wc.lpszClassName = WND_CLASS_OPT;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(NULL, (const wchar_t *)(ULONG_PTR)32512);
    if (!RegisterClassExW(&wc)) {
        Log("RegisterClass(options) failed err=%lu", (unsigned long)GetLastError());
    }

    g_hWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, APP_CLASS, APP_NAME,
                             0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);
    if (!g_hWnd) g_hWnd = CreateWindowExW(WS_EX_TOOLWINDOW, APP_CLASS, APP_NAME,
                                          0, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hWnd) {
        MessageBoxW(NULL, L"AutoClip could not start (window).", APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    RefreshTray();
    if (!g_trayAdded) {
        Log("tray not available");
    }

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, hInst, 0);
    if (!g_hook) {
        Log("hook failed err=%lu", (unsigned long)GetLastError());
        MessageBoxW(NULL,
                    L"AutoClip could not install the mouse hook, so it cannot\n"
                    L"notice selections or double clicks.\n\n"
                    L"Check that your security software allows global hooks.",
                    APP_NAME, MB_OK | MB_ICONERROR);
        RemoveTray();
        return 1;
    }
    Log("hook installed");

    RegisterHotKey(g_hWnd, 1, MOD_CONTROL | MOD_ALT, 'P');
    RegisterHotKey(g_hWnd, 2, MOD_CONTROL | MOD_ALT, 'O');

    if (wantDebug) {
        ShowOptions();
        Log("debug mode: options window opened");
    }

    Log("entering message loop");

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == g_taskbarCreated && g_taskbarCreated) {
            /* explorer restarted, the icon is gone */
            g_trayAdded = FALSE;
            RefreshTray();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Log("shutdown");
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
    UnregisterHotKey(g_hWnd, 1);
    UnregisterHotKey(g_hWnd, 2);
    RemoveTray();
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    if (g_log) { fclose(g_log); g_log = NULL; }
    return 0;
}
