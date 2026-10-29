#include <windows.h>
#include <stdio.h>
int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR c, int s)
{
    HICON a = LoadIconW(hi, (const wchar_t *)(ULONG_PTR)101);
    HICON b = LoadIconW(hi, (const wchar_t *)(ULONG_PTR)32512);
    FILE *f = fopen("iconcheck.txt", "w");
    if (f) { fprintf(f, "res101=%p idi_app=%p err=%lu\n", (void*)a, (void*)b, (unsigned long)GetLastError()); fclose(f); }
    (void)hp; (void)c; (void)s;
    return 0;
}