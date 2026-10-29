/*
 * Logical checks for AutoClip's gesture state machine and clipboard helpers.
 *
 * The hooks, SendInput and the clipboard cannot be exercised reliably inside a
 * service session, so the parts that decide "what to do" are tested here
 * directly: gesture arming/cancellation, the smart-mode decision, and the
 * clipboard readers used to tell a real selection from an empty one.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static FILE *g_out;
static int g_fail;

static void W(const wchar_t *s)
{
    const wchar_t *p;
    if (!g_out) return;
    for (p = s; *p; p++) fputc(*p < 128 ? (int)*p : '?', g_out);
}

static void Say(const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    if (!g_out) return;
    va_start(ap, fmt);
    vswprintf(buf, fmt, ap);
    va_end(ap);
    W(buf);
    fputc('\n', g_out);
    fflush(g_out);
}

static void Check(const char *name, int ok, const char *detail)
{
    Say(L"%-34hs : %ls   %hs", name, ok ? L"PASS" : L"FAIL", detail);
    if (!ok) g_fail = 1;
}

/* ---- copies of the logic under test ------------------------------------- */

#define DRAG_TOLERANCE 4
#define GESTURE_NONE 0
#define GESTURE_DRAG 1
#define GESTURE_MULTI 2

typedef struct {
    DWORD lastDownTick;
    POINT lastDownPt;
    POINT anchorPt;
    int   clickCount;
    BOOL  dragging;
    int   armed;
    int   armedDouble;
} Gesture;

static void G_Reset(Gesture *g)
{
    memset(g, 0, sizeof(*g));
}

static void G_Down(Gesture *g, POINT pt, DWORD now)
{
    int tol = GetSystemMetrics(SM_CXDOUBLECLK);
    int dx = pt.x - g->lastDownPt.x;
    int dy = pt.y - g->lastDownPt.y;

    if (tol < 4) tol = 4;
    g->armed = GESTURE_NONE;
    if (now - g->lastDownTick <= 300 && dx <= tol && dx >= -tol && dy <= tol && dy >= -tol)
        g->clickCount++;
    else
        g->clickCount = 1;
    g->lastDownTick = now;
    g->lastDownPt = pt;
    g->anchorPt = pt;
    g->dragging = FALSE;
}

static void G_Move(Gesture *g, POINT pt)
{
    int dx, dy;
    if (g->clickCount <= 0 || g->dragging) return;
    dx = pt.x - g->anchorPt.x;
    dy = pt.y - g->anchorPt.y;
    if (dx > DRAG_TOLERANCE || dx < -DRAG_TOLERANCE || dy > DRAG_TOLERANCE || dy < -DRAG_TOLERANCE)
        g->dragging = TRUE;
}

static int G_Up(Gesture *g)
{
    int gesture = GESTURE_NONE;

    if (g->dragging) gesture = GESTURE_DRAG;
    else if (g->clickCount >= 2) gesture = GESTURE_MULTI;
    g->dragging = FALSE;
    if (gesture != GESTURE_NONE) {
        g->armed = gesture;
        g->armedDouble = (gesture == GESTURE_MULTI);
    }
    return gesture;
}

/*
 * Smart-mode decision as implemented in DoDoubleGesture():
 *   selection came back  -> copy only, never paste
 *   nothing selected     -> paste
 */
static int SmartDecision(int hasSelection)
{
    return hasSelection ? 0 : 1;      /* 1 = paste */
}

/* clipboard helpers */
static BOOL ClipHasText(void)
{
    HANDLE h;
    wchar_t *p;
    BOOL ok = FALSE;
    int i;

    for (i = 0; i < 8; i++) { if (OpenClipboard(NULL)) break; Sleep(8); }
    if (i == 8) return FALSE;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        p = (wchar_t *)GlobalLock(h);
        if (p) {
            while (*p) {
                if (*p != L' ' && *p != L'\t' && *p != L'\r' && *p != L'\n' && *p != (wchar_t)0x3000)
                { ok = TRUE; break; }
                p++;
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return ok;
}

static void ClipSet(const wchar_t *s)
{
    HGLOBAL h;
    wchar_t *p;
    int i;

    for (i = 0; i < 8; i++) { if (OpenClipboard(NULL)) break; Sleep(8); }
    if (i == 8) return;
    EmptyClipboard();
    h = GlobalAlloc(GMEM_MOVEABLE, (wcslen(s) + 1) * sizeof(wchar_t));
    if (h) {
        p = (wchar_t *)GlobalLock(h);
        if (p) { wcscpy(p, s); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
    }
    CloseClipboard();
}

int main(void)
{
    Gesture g;
    POINT a, b;

    g_out = _wfopen(L"logic_out.txt", L"w");
    if (!g_out) return 1;

    /* 1. a plain click is not a gesture */
    G_Reset(&g);
    a.x = 100; a.y = 100;
    G_Down(&g, a, 1000);
    Check("single click -> no gesture", G_Up(&g) == GESTURE_NONE, "");

    /* 2. drag inside the tolerance is still a click */
    G_Reset(&g);
    G_Down(&g, a, 1000);
    b.x = 102; b.y = 101;
    G_Move(&g, b);
    Check("2px move -> still click", G_Up(&g) == GESTURE_NONE, "");

    /* 3. a real drag arms the copy gesture */
    G_Reset(&g);
    G_Down(&g, a, 1000);
    b.x = 160; b.y = 118;
    G_Move(&g, b);
    Check("drag -> copy gesture", G_Up(&g) == GESTURE_DRAG, "");

    /* 4. two quick clicks in the same place arm the double-click gesture */
    G_Reset(&g);
    G_Down(&g, a, 1000);
    G_Up(&g);
    b.x = 101; b.y = 100;
    G_Down(&g, b, 1120);
    Check("double click -> paste gesture", G_Up(&g) == GESTURE_MULTI, "");

    /* 5. two slow clicks are two single clicks */
    G_Reset(&g);
    G_Down(&g, a, 1000);
    G_Up(&g);
    G_Down(&g, b, 1600);
    Check("slow second click -> no gesture", G_Up(&g) == GESTURE_NONE, "");

    /* 6. a click far away is not a double click */
    G_Reset(&g);
    G_Down(&g, a, 1000);
    G_Up(&g);
    b.x = 400; b.y = 400;
    G_Down(&g, b, 1100);
    Check("click elsewhere -> no gesture", G_Up(&g) == GESTURE_NONE, "");

    /* 7. a new press cancels a pending gesture (the 200 ms window) */
    G_Reset(&g);
    G_Down(&g, a, 1000);
    b.x = 160; b.y = 118;
    G_Move(&g, b);
    G_Up(&g);
    b.x = 300; b.y = 300;
    G_Down(&g, b, 1050);
    Check("press during 200ms window cancels", g.armed == GESTURE_NONE, "");

    /* 8. smart mode: an empty editor double click pastes */
    Check("smart: empty -> paste", SmartDecision(0) == 1, "");
    /* 9. smart mode: a word was selected, no paste */
    Check("smart: word selected -> no paste", SmartDecision(1) == 0, "");

    /* 10. clipboard helpers */
    ClipSet(L"   \r\n\t  ");
    Check("whitespace is not a selection", !ClipHasText(), "");
    ClipSet(L"\x4e2d\x6587 selection");
    Check("CJK text is a selection", ClipHasText(), "");
    ClipSet(L"");
    Check("empty clipboard is not a selection", !ClipHasText(), "");

    Say(L"RESULT : %ls", g_fail ? L"FAIL" : L"ALL PASS");
    fclose(g_out);
    return g_fail;
}
