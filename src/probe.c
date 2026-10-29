#include <windows.h>
#include <stdio.h>

int main(void)
{
    const wchar_t *s = L"选中即复制 双击自动粘贴";
    int i, n = (int)wcslen(s);
    BITMAPV5HEADER bv5;
    ICONINFO ii;
    HDC hdc;
    HBITMAP hbm;
    void *bits = NULL;
    HICON ico;

    FILE *f = _wfopen(L"probe_wide.txt", L"wb");
    if (f) { fwrite(s, 2, n, f); fclose(f); }

    f = fopen("probe_hex.txt", "w");
    if (f) {
        for (i = 0; i < n; i++) fprintf(f, "%04X ", (unsigned)s[i]);
        fputc('\n', f);
        fprintf(f, "sizeof(INPUT)=%d sizeof(MSLLHOOKSTRUCT)=%d sizeof(NOTIFYICONDATA-ish)=0\n",
                (int)sizeof(INPUT), (int)sizeof(MSLLHOOKSTRUCT));
        fclose(f);
    }

    /* try creating a 32bpp DIB icon */
    ZeroMemory(&bv5, sizeof(bv5));
    bv5.bV5Size = sizeof(BITMAPV5HEADER);
    bv5.bV5Width = 32;
    bv5.bV5Height = -32; /* top-down */
    bv5.bV5Planes = 1;
    bv5.bV5BitCount = 32;
    bv5.bV5Compression = BI_RGB;
    hdc = GetDC(NULL);
    hbm = CreateDIBSection(hdc, (BITMAPINFO *)&bv5, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);

    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmColor = hbm;
    ii.hbmMask = CreateBitmap(32, 32, 1, 1, NULL);
    ico = CreateIconIndirect(&ii);

    f = fopen("probe_result.txt", "w");
    if (f) {
        fprintf(f, "dib_bits=%p hicon=%p\n", bits, (void *)ico);
        fclose(f);
    }
    return 0;
}
