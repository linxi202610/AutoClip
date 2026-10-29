/* 模拟真实鼠标手势：一次拖选 + 一次双击，用来验证 AutoClip 的手势识别 */
#include <windows.h>
#include <stdio.h>

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

static void Btn(DWORD flags)
{
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof(INPUT));
    Sleep(15);
}

int main(void)
{
    int cx = GetSystemMetrics(SM_CXSCREEN);
    int cy = GetSystemMetrics(SM_CYSCREEN);
    POINT old;
    int x0, y0;
    FILE *f;

    GetCursorPos(&old);

    x0 = cx / 2;
    y0 = cy / 2 + cy / 6;
    if (y0 > cy - 100) y0 = cy - 100;

    /* ---- 手势 1：拖选 ---- */
    MoveAbs(x0, y0);
    Sleep(80);
    Btn(MOUSEEVENTF_LEFTDOWN);
    MoveAbs(x0 + 40, y0 + 6);
    MoveAbs(x0 + 120, y0 + 12);
    MoveAbs(x0 + 200, y0 + 16);
    Btn(MOUSEEVENTF_LEFTUP);
    Sleep(700);

    /* ---- 手势 2：双击 ---- */
    MoveAbs(x0, y0 + 90);
    Sleep(60);
    Btn(MOUSEEVENTF_LEFTDOWN);
    Btn(MOUSEEVENTF_LEFTUP);
    Sleep(55);
    Btn(MOUSEEVENTF_LEFTDOWN);
    Btn(MOUSEEVENTF_LEFTUP);
    Sleep(700);

    SetCursorPos(old.x, old.y);

    f = fopen("sim_done.txt", "w");
    if (f) {
        fprintf(f, "done screen=%dx%d point=%d,%d\n", cx, cy, x0, y0);
        fclose(f);
    }
    return 0;
}
