/*
 * AutoClip - 选中即复制 / 双击粘贴
 *
 * 机制：
 *   1) WH_MOUSE_LL 全局鼠标钩子识别两类手势：拖选（按住移动 > 5px）、
 *      双击/三击（间隔 < 系统双击时间且位置接近）。
 *   2) 手势结束后不起 200ms 定时器；期间任何新的按下都会取消它，
 *      所以连击不会重复触发。
 *   3) 定时器到点后发送 Ctrl+C。若手势是双击且开启了粘贴，
 *      再延时 170ms 发送 Ctrl+V（等目标程序把剪贴板写完）。
 *   4) 智能模式下先比对剪贴板序列号，序列号没变说明双击没有产生选区，
 *      此时才粘贴，避免打断“双击选词”。
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>

/* ---------------- shellapi 缺失部分（TCC 无 shellapi.h） ---------------- */
#define NIM_ADD     0
#define NIM_MODIFY  1
#define NIM_DELETE  2

#define NIF_MESSAGE 0x00000001
#define NIF_ICON    0x00000002
#define NIF_TIP     0x00000004
#define NIF_INFO    0x00000010

#define NIIF_INFO   0x00000001

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

/* ---------------------------- 常量 ---------------------------- */
#define APP_NAME        L"AutoClip"
#define APP_CLASS       L"AutoClipHiddenWindow"
#define TRAY_UID        1
#define WM_TRAYICON     (WM_APP + 1)

#define TIMER_ACT       1
#define TIMER_PASTE     2

#define IDM_COPY        101
#define IDM_PASTE       102
#define IDM_SMART       103
#define IDM_STARTUP     104
#define IDM_ABOUT       105
#define IDM_EXIT        106

#define GESTURE_NONE    0
#define GESTURE_DRAG    1
#define GESTURE_MULTI   2

#define GAP_MS          200   /* 松手后等待多久自动复制 */
#define PASTE_DELAY_MS  170   /* 复制后再等多久粘贴 */
#define DRAG_TOLERANCE  5     /* 超过这么多像素算拖选 */

#define REG_SUBKEY      L"Software\\AutoClip"
#define REG_RUNKEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

/* ---------------------------- 全局状态 ---------------------------- */
static HINSTANCE g_hInst;
static HWND      g_hWnd;
static HHOOK     g_hook;
static HICON     g_trayIcon;
static BOOL      g_trayAdded;

static BOOL g_copyEnabled = TRUE;
static BOOL g_pasteEnabled = TRUE;
static BOOL g_smartMode   = TRUE;
static BOOL g_autoStart   = FALSE;

static DWORD g_lastDownTick;
static POINT g_lastDownPt;
static POINT g_anchorPt;
static int   g_clickCount;
static BOOL  g_dragging;
static int   g_pendingGesture;
static DWORD g_seqBefore;
static wchar_t *g_savedClip;
static int   g_pasteRetry;

static FILE *g_log;

/* ---------------------------- 日志 ---------------------------- */
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

static void LogOpen(void)
{
    wchar_t dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    DWORD n;

    n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) return;
    lstrcpynW(path, dir, MAX_PATH);
    lstrcatW(path, L"AutoClip.log");
    g_log = _wfopen(path, L"w");
    Log("AutoClip started");
}

/* ---------------------------- 注册表 ---------------------------- */
static DWORD RegGetDword(HKEY root, const wchar_t *sub, const wchar_t *name, DWORD def)
{
    HKEY k;
    DWORD v = def, cb, type;

    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return def;

    cb = sizeof(v);
    type = 0;
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)&v, &cb) != ERROR_SUCCESS || type != REG_DWORD)
        v = def;
    RegCloseKey(k);
    return v;
}

static void RegSetDword(HKEY root, const wchar_t *sub, const wchar_t *name, DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
}

static BOOL QueryAutoStart(void)
{
    HKEY k;
    wchar_t buf[MAX_PATH + 16];
    DWORD cb = sizeof(buf);
    BOOL found = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUNKEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"AutoClip", NULL, NULL, (BYTE *)buf, &cb) == ERROR_SUCCESS)
            found = TRUE;
        RegCloseKey(k);
    }
    return found;
}

static void ApplyAutoStart(BOOL on)
{
    HKEY k;
    wchar_t path[MAX_PATH + 8];

    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_RUNKEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)
        != ERROR_SUCCESS)
        return;

    if (on) {
        path[0] = L'"';
        if (GetModuleFileNameW(NULL, path + 1, MAX_PATH) > 0) {
            lstrcatW(path, L"\"");
            RegSetValueExW(k, L"AutoClip", 0, REG_SZ,
                           (const BYTE *)path, (DWORD)((lstrlenW(path) + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(k, L"AutoClip");
    }
    RegCloseKey(k);
}

static void LoadSettings(void)
{
    g_copyEnabled  = RegGetDword(HKEY_CURRENT_USER, REG_SUBKEY, L"CopyEnabled", 1) ? TRUE : FALSE;
    g_pasteEnabled = RegGetDword(HKEY_CURRENT_USER, REG_SUBKEY, L"PasteEnabled", 1) ? TRUE : FALSE;
    g_smartMode    = RegGetDword(HKEY_CURRENT_USER, REG_SUBKEY, L"SmartMode", 1) ? TRUE : FALSE;
    g_autoStart    = QueryAutoStart();
}

static void SaveSettings(void)
{
    RegSetDword(HKEY_CURRENT_USER, REG_SUBKEY, L"CopyEnabled", g_copyEnabled ? 1 : 0);
    RegSetDword(HKEY_CURRENT_USER, REG_SUBKEY, L"PasteEnabled", g_pasteEnabled ? 1 : 0);
    RegSetDword(HKEY_CURRENT_USER, REG_SUBKEY, L"SmartMode", g_smartMode ? 1 : 0);
    RegSetDword(HKEY_CURRENT_USER, REG_SUBKEY, L"Installed", 1);
}

/* ---------------------------- 托盘图标 ---------------------------- */
/* 和 exe 的 .ico 保持同一套视觉：圆角方块 + 白色对勾，用颜色表示开关状态 */

static BOOL PointInRoundedRect(int x, int y, int w, int h, int r)
{
    int cx, cy, dx, dy;

    if (x >= r && x < w - r) return TRUE;
    if (y >= r && y < h - r) return TRUE;

    cx = (x < r) ? r : (w - 1 - r);
    cy = (y < r) ? r : (h - 1 - r);
    dx = x - cx;
    dy = y - cy;
    return (dx * dx + dy * dy <= r * r);
}

static double SegDist(double px, double py, double x1, double y1, double x2, double y2)
{
    double dx = x2 - x1, dy = y2 - y1;
    double l2 = dx * dx + dy * dy;
    double t, ex, ey;

    if (l2 == 0.0) {
        ex = px - x1;
        ey = py - y1;
    } else {
        t = ((px - x1) * dx + (py - y1) * dy) / l2;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        ex = px - (x1 + t * dx);
        ey = py - (y1 + t * dy);
    }
    return sqrt(ex * ex + ey * ey);
}

static HICON MakeIcon(BYTE r, BYTE g, BYTE b)
{
    enum { S = 32 };
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
    bi.bV5Compression = BI_RGB;

    hdc = GetDC(NULL);
    hbmColor = CreateDIBSection(hdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!hbmColor || !bits)
        return NULL;

    px = (BYTE *)bits;
    for (y = 0; y < S; y++) {
        for (x = 0; x < S; x++) {
            BYTE *p = px + (y * S + x) * 4;

            if (!PointInRoundedRect(x, y, S, S, 8)) {
                p[0] = p[1] = p[2] = 0;
                p[3] = 0;
                continue;
            }

            {
                double t = (double)y / (S - 1);
                double k = 1.0 - 0.30 * t;          /* 向下略微加深 */
                double d = SegDist(x, y, 9.0, 17.2, 14.2, 22.4);

                if (d > 3.0)
                    d = SegDist(x, y, 14.2, 22.4, 23.0, 10.2);

                if (d <= 3.0) {
                    p[0] = 255; p[1] = 255; p[2] = 255; p[3] = 255;
                } else {
                    p[0] = (BYTE)(b * k);
                    p[1] = (BYTE)(g * k);
                    p[2] = (BYTE)(r * k);
                    p[3] = 255;
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
    return ico;
}

static void RefreshTray(void)
{
    NIDATA nid;
    HICON ico;
    BYTE r, g, b;

    if (!g_copyEnabled && !g_pasteEnabled) {
        r = 0x9C; g = 0xA3; b = 0xAF;      /* 灰：全部关闭 */
    } else if (g_copyEnabled && g_pasteEnabled) {
        r = 0x22; g = 0xC5; b = 0x5E;      /* 绿：都开 */
    } else {
        r = 0xF5; g = 0x9E; b = 0x0B;      /* 琥珀：只开一个 */
    }

    ico = MakeIcon(r, g, b);

    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(NIDATA);
    nid.hWnd = g_hWnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = ico;

    lstrcpynW(nid.szTip, L"AutoClip", 128);
    lstrcatW(nid.szTip, g_copyEnabled ? L"｜选中即复制：开" : L"｜选中即复制：关");
    lstrcatW(nid.szTip, g_pasteEnabled ? L"｜双击粘贴：开" : L"｜双击粘贴：关");
    lstrcatW(nid.szTip, L"\n单击查看状态，右键打开菜单");

    if (g_trayAdded)
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    else {
        if (Shell_NotifyIconW(NIM_ADD, &nid))
            g_trayAdded = TRUE;
        else
            Log("NIM_ADD failed err=%lu", (unsigned long)GetLastError());
    }

    if (g_trayIcon && g_trayIcon != ico)
        DestroyIcon(g_trayIcon);
    g_trayIcon = ico;
    Log("tray refreshed copy=%d paste=%d", (int)g_copyEnabled, (int)g_pasteEnabled);
}

static void ShowBalloon(const wchar_t *title, const wchar_t *text)
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
    Shell_NotifyIconW(NIM_MODIFY, &nid);
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

/* ---------------------------- 剪贴板快照 ---------------------------- */
static wchar_t *ClipReadText(void)
{
    HANDLE h;
    wchar_t *p, *out = NULL;
    int tries = 0;

    while (!OpenClipboard(NULL)) {
        if (++tries > 6) return NULL;
        Sleep(10);
    }

    h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        p = (wchar_t *)GlobalLock(h);
        if (p) {
            int n = lstrlenW(p);
            out = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
            if (out) lstrcpyW(out, p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

static BOOL ClipWriteText(const wchar_t *s)
{
    int tries = 0;
    int n;
    HGLOBAL h;
    wchar_t *p;
    BOOL ok = FALSE;

    if (!s) return FALSE;
    n = lstrlenW(s);

    while (!OpenClipboard(NULL)) {
        if (++tries > 6) return FALSE;
        Sleep(10);
    }

    if (EmptyClipboard()) {
        h = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(wchar_t));
        if (h) {
            p = (wchar_t *)GlobalLock(h);
            if (p) {
                lstrcpyW(p, s);
                GlobalUnlock(h);
                if (SetClipboardData(CF_UNICODETEXT, h))
                    ok = TRUE;
            }
        }
    }
    CloseClipboard();
    return ok;
}

/* 判断复制到的内容是否“有实质文字”。很多控件在无选区时按 Ctrl+C，
   会往剪贴板里塞一个空格或空串，这种情况不能算“选中了东西”。 */
static BOOL HasMeaningfulText(const wchar_t *s)
{
    if (!s) return FALSE;
    while (*s) {
        if (*s != L' ' && *s != L'\t' && *s != L'\r' && *s != L'\n' && *s != (wchar_t)0x3000)
            return TRUE;
        s++;
    }
    return FALSE;
}

/* ---------------------------- 键盘注入 ---------------------------- */
static void SendCtrlKey(WORD vk)
{
    INPUT in[4];
    int n = 0;

    ZeroMemory(in, sizeof(in));
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_CONTROL; n++;
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk;          n++;
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk;          in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_CONTROL; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;

    if (SendInput(n, in, sizeof(INPUT)) != (UINT)n)
        Log("SendInput failed err=%lu", (unsigned long)GetLastError());
}

/* ---------------------------- 定时器回调 ---------------------------- */
static void OnActTimer(void)
{
    int gesture = g_pendingGesture;
    BOOL wantPaste;

    KillTimer(g_hWnd, TIMER_ACT);
    g_pendingGesture = GESTURE_NONE;
    if (gesture == GESTURE_NONE) return;

    wantPaste = (gesture == GESTURE_MULTI && g_pasteEnabled);
    g_pasteRetry = 0;

    Log("gesture=%d copy=%d paste=%d smart=%d", gesture,
        (int)g_copyEnabled, (int)g_pasteEnabled, (int)g_smartMode);

    /* 强制粘贴模式：不做选区探测，直接贴，绝不碰用户剪贴板 */
    if (wantPaste && !g_smartMode) {
        SendCtrlKey('V');
        Log("force paste sent");
        return;
    }

    if (g_copyEnabled) {
        /* 智能模式且要粘贴：先给剪贴板拍个快照，供后面判断/恢复 */
        if (wantPaste) {
            if (g_savedClip) {
                free(g_savedClip);
                g_savedClip = NULL;
            }
            g_savedClip = ClipReadText();
        }
        g_seqBefore = GetClipboardSequenceNumber();
        SendCtrlKey('C');

        if (wantPaste)
            SetTimer(g_hWnd, TIMER_PASTE, PASTE_DELAY_MS, NULL);
    } else if (wantPaste) {
        SendCtrlKey('V');
    }
}

static void OnPasteTimer(void)
{
    wchar_t *cur;
    BOOL gotSelection;

    KillTimer(g_hWnd, TIMER_PASTE);
    if (!g_pasteEnabled) return;

    if (!g_smartMode) {
        SendCtrlKey('V');
        return;
    }

    cur = ClipReadText();

    /* 剪贴板还没被目标程序写完，再等一会儿 */
    if (cur && cur[0] == 0 && g_pasteRetry < 2) {
        g_pasteRetry++;
        free(cur);
        SetTimer(g_hWnd, TIMER_PASTE, 90, NULL);
        return;
    }

    gotSelection = (HasMeaningfulText(cur)
                    && (g_savedClip == NULL || lstrcmpW(cur, g_savedClip) != 0));

    Log("smart: cur_len=%d saved_len=%d selection=%d",
        cur ? (int)lstrlenW(cur) : -1,
        g_savedClip ? (int)lstrlenW(g_savedClip) : -1,
        (int)gotSelection);

    if (gotSelection) {
        /* 双击落在了文字上，那就是“选词”，只保留复制结果，不粘贴 */
        Log("smart: selection captured, paste skipped");
    } else {
        /* 没有选区：把用户原来的剪贴板还原，再粘贴 */
        if (g_savedClip)
            ClipWriteText(g_savedClip);
        SendCtrlKey('V');
        Log("smart: no selection, clipboard restored, paste sent");
    }

    if (cur) free(cur);
    if (g_savedClip) {
        free(g_savedClip);
        g_savedClip = NULL;
    }
}

/* ---------------------------- 鼠标钩子 ---------------------------- */
static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lParam;

        switch (wParam) {
        case WM_LBUTTONDOWN:
            KillTimer(g_hWnd, TIMER_ACT);
            KillTimer(g_hWnd, TIMER_PASTE);
            {
                DWORD now = GetTickCount();
                int tol = GetSystemMetrics(SM_CXDRAG);
                int dx = ms->pt.x - g_lastDownPt.x;
                int dy = ms->pt.y - g_lastDownPt.y;

                if (tol < 2) tol = 2;
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
                int gesture = GESTURE_NONE;

                if (g_dragging)
                    gesture = GESTURE_DRAG;
                else if (g_clickCount >= 2)
                    gesture = GESTURE_MULTI;

                g_dragging = FALSE;

                if (gesture != GESTURE_NONE && (g_copyEnabled || g_pasteEnabled)) {
                    g_pendingGesture = gesture;
                    SetTimer(g_hWnd, TIMER_ACT, GAP_MS, NULL);
                }
            }
            break;

        case WM_RBUTTONDOWN:
            KillTimer(g_hWnd, TIMER_ACT);
            KillTimer(g_hWnd, TIMER_PASTE);
            g_clickCount = 0;
            break;

        default:
            break;
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

/* ---------------------------- 菜单 ---------------------------- */
static void ShowAbout(void)
{
    MessageBoxW(NULL,
        L"AutoClip  ——  选中即复制 / 双击粘贴\n"
        L"\n"
        L"【功能一】选中即复制\n"
        L"  拖选文字、图片后松开鼠标，停留 200 毫秒，程序自动\n"
        L"  替你按下 Ctrl+C，不用再去摸键盘。双击选中一个词\n"
        L"  也同样会自动复制。\n"
        L"\n"
        L"【功能二】双击自动粘贴\n"
        L"  在 Word、记事本、浏览器输入框等任何能输入的地方双击\n"
        L"  鼠标左键，程序自动按下 Ctrl+V。\n"
        L"\n"
        L"【为什么有「智能模式」】\n"
        L"  Windows 里双击的本来含义是“选中一个词”，和“双击粘贴”\n"
        L"  天生冲突，只能二选一。\n"
        L"\n"
        L"  勾选智能模式（默认）时：\n"
        L"    双击文字上  → 当成选词，只复制，不粘贴；\n"
        L"    双击空白处  → 当成粘贴，而且会把你原来的剪贴板内容\n"
        L"                  原样还给你再粘贴，不会污染剪贴板。\n"
        L"  取消勾选时：双击一律粘贴，不管有没有选中文字。\n"
        L"\n"
        L"【托盘图标】\n"
        L"  绿色 = 两个功能都开；琥珀色 = 只开一个；灰色 = 全关。\n"
        L"  左键单击 = 查看状态；右键 = 开关菜单。\n"
        L"\n"
        L"【说明】\n"
        L"  设置保存在 HKEY_CURRENT_USER\\Software\\AutoClip，重启不丢。\n"
        L"  若目标程序以管理员身份运行，本程序也要用管理员身份运行，\n"
        L"  否则 Windows 不允许把按键送进高权限窗口。\n",
        L"AutoClip  使用说明",
        MB_OK | MB_ICONINFORMATION);
}

static void ShowTrayMenu(void)
{
    HMENU m;
    POINT pt;
    HWND hFore;
    DWORD tidFore, tidMe;

    GetCursorPos(&pt);

    m = CreatePopupMenu();
    if (!m) return;

    AppendMenuW(m, MF_STRING | (g_copyEnabled ? MF_CHECKED : 0), IDM_COPY,
                L"选中即复制（停 200 ms）");
    AppendMenuW(m, MF_STRING | (g_pasteEnabled ? MF_CHECKED : 0), IDM_PASTE,
                L"双击自动粘贴");
    AppendMenuW(m, MF_STRING | (g_smartMode ? MF_CHECKED : 0), IDM_SMART,
                L"    智能模式：双击选中文字时不粘贴");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_autoStart ? MF_CHECKED : 0), IDM_STARTUP,
                L"开机自动启动");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_ABOUT, L"使用说明");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");

    SetMenuDefaultItem(m, IDM_ABOUT, FALSE);

    hFore = GetForegroundWindow();
    tidFore = GetWindowThreadProcessId(hFore, NULL);
    tidMe = GetCurrentThreadId();
    if (tidFore != 0 && tidFore != tidMe)
        AttachThreadInput(tidMe, tidFore, TRUE);
    SetForegroundWindow(g_hWnd);
    if (tidFore != 0 && tidFore != tidMe)
        AttachThreadInput(tidMe, tidFore, FALSE);

    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                   pt.x, pt.y, 0, g_hWnd, NULL);
    PostMessage(g_hWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ---------------------------- 窗口过程 ---------------------------- */
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
            {
                wchar_t buf[256];
                lstrcpynW(buf, L"选中即复制：", 256);
                lstrcatW(buf, g_copyEnabled ? L"开启" : L"关闭");
                lstrcatW(buf, L"\n双击自动粘贴：");
                lstrcatW(buf, g_pasteEnabled ? L"开启" : L"关闭");
                lstrcatW(buf, L"\n（右键托盘图标可切换）");
                ShowBalloon(APP_NAME, buf);
            }
            return 0;
        }
        break;

    case WM_TIMER:
        if (wp == TIMER_ACT)
            OnActTimer();
        else if (wp == TIMER_PASTE)
            OnPasteTimer();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_COPY:
            g_copyEnabled = !g_copyEnabled;
            SaveSettings();
            RefreshTray();
            return 0;
        case IDM_PASTE:
            g_pasteEnabled = !g_pasteEnabled;
            SaveSettings();
            RefreshTray();
            return 0;
        case IDM_SMART:
            g_smartMode = !g_smartMode;
            SaveSettings();
            return 0;
        case IDM_STARTUP:
            g_autoStart = !g_autoStart;
            ApplyAutoStart(g_autoStart);
            return 0;
        case IDM_ABOUT:
            ShowAbout();
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------------------- 入口 ---------------------------- */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    WNDCLASSEXW wc;
    MSG msg;
    HANDLE mtx;

    (void)hPrev;
    (void)lpCmd;
    (void)nShow;
    g_hInst = hInst;

    if (wcsstr(GetCommandLineW(), L"--debug") != NULL)
        LogOpen();

    mtx = CreateMutexW(NULL, TRUE, L"Local\\AutoClip_SingleInstance");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL,
                    L"AutoClip 已经在运行了。\n\n请到屏幕右下角的托盘区找它（绿色的圆点图标）。",
                    APP_NAME, MB_OK | MB_ICONINFORMATION);
        CloseHandle(mtx);
        return 0;
    }

    LoadSettings();
    Log("settings loaded: copy=%d paste=%d smart=%d autostart=%d",
        (int)g_copyEnabled, (int)g_pasteEnabled, (int)g_smartMode, (int)g_autoStart);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = APP_CLASS;
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"窗口类注册失败，程序无法启动。", APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    g_hWnd = CreateWindowExW(0, APP_CLASS, APP_NAME, WS_OVERLAPPEDWINDOW,
                             0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hWnd) {
        MessageBoxW(NULL, L"窗口创建失败，程序无法启动。", APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    RefreshTray();
    if (!g_trayAdded) {
        MessageBoxW(NULL,
                    L"托盘图标创建失败，程序无法正常工作。\n\n"
                    L"这通常说明系统的通知区域被禁用或被安全软件拦截。",
                    APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, hInst, 0);
    if (!g_hook) {
        Log("SetWindowsHookEx failed err=%lu", (unsigned long)GetLastError());
        MessageBoxW(NULL,
                    L"鼠标钩子安装失败，程序无法监听选区和双击。\n\n"
                    L"请检查安全软件是否拦截了全局钩子。",
                    APP_NAME, MB_OK | MB_ICONERROR);
        RemoveTray();
        return 1;
    }
    Log("hook installed ok");

    if (RegGetDword(HKEY_CURRENT_USER, REG_SUBKEY, L"Installed", 0) == 0) {
        ShowBalloon(L"AutoClip 已启动",
                    L"选中文字或图片，松开鼠标停 0.2 秒即自动复制；"
                    L"在输入框或空白处双击即自动粘贴。"
                    L"右键托盘图标可关闭功能或查看说明。");
        SaveSettings();
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Log("shutting down");
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }
    RemoveTray();
    if (g_trayIcon) {
        DestroyIcon(g_trayIcon);
        g_trayIcon = NULL;
    }
    if (mtx) {
        ReleaseMutex(mtx);
        CloseHandle(mtx);
    }
    if (g_log) fclose(g_log);
    return 0;
}
