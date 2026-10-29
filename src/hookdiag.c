#include <windows.h>
#include <stdio.h>
static FILE *g;
static HHOOK g_h;
static LRESULT CALLBACK Proc(int n, WPARAM w, LPARAM l)
{
    if (n == HC_ACTION && g) { fprintf(g, "evt %u\n", (unsigned)w); fflush(g); }
    return CallNextHookEx(g_h, n, w, l);
}
int main(void)
{
    MSG m;
    g = fopen("hook_diag.txt", "w");
    g_h = SetWindowsHookExW(WH_MOUSE_LL, Proc, GetModuleHandleW(NULL), 0);
    if (!g_h) { if (g) { fprintf(g, "hook install failed %lu\n", (unsigned long)GetLastError()); fclose(g);} return 1; }
    if (g) { fprintf(g, "hook ok\n"); fflush(g); }
    Sleep(200);
    /* synthetic mouse move to generate an event */
    {
        INPUT in; ZeroMemory(&in, sizeof(in));
        in.type = INPUT_MOUSE; in.mi.dx = 10; in.mi.dy = 10; in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof(INPUT));
    }
    Sleep(300);
    UnhookWindowsHookEx(g_h);
    if (g) { fprintf(g, "done\n"); fclose(g); }
    return 0;
}