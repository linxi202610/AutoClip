#include <windows.h>
#include <stdio.h>
static FILE *g;
static HHOOK g_h;
static DWORD g_first;
static LRESULT CALLBACK Proc(int n, WPARAM w, LPARAM l)
{
    if (n == HC_ACTION) {
        MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT*)l;
        if (g) { fprintf(g, "evt w=%u pt=%ld,%ld flags=%lx\n", (unsigned)w, ms->pt.x, ms->pt.y, (unsigned long)ms->flags); fflush(g); }
    }
    return CallNextHookEx(g_h, n, w, l);
}
int main(void)
{
    MSG m;
    int i;
    g = fopen("hook_diag2.txt", "w");
    g_h = SetWindowsHookExW(WH_MOUSE_LL, Proc, GetModuleHandleW(NULL), 0);
    if (g) { fprintf(g, "hook=%p err=%lu\n", (void*)g_h, (unsigned long)GetLastError()); fflush(g); }
    for (i = 0; i < 300; i++) {
        INPUT in; ZeroMemory(&in, sizeof(in));
        in.type = INPUT_MOUSE;
        in.mi.dx = 5 + (i % 7); in.mi.dy = 3 + (i % 5);
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof(INPUT));
        Sleep(20);
        if (g) { fprintf(g, "sent %d\n", i); fflush(g); }
    }
    UnhookWindowsHookEx(g_h);
    if (g) { fprintf(g, "done\n"); fclose(g); }
    return 0;
}