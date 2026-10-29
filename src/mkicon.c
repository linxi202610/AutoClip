/*
 * mkicon - writes an AutoClip .ico with 16/24/32/48/64/128/256 px entries.
 * Pure Win32 + libtcc1, no imaging library needed.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    int w, h;
    unsigned char *px;   /* RGBA, top-down */
} Canvas;

static int InRounded(int x, int y, int s, int r)
{
    int cx, cy;

    if (x >= r && x < s - r) return 1;
    if (y >= r && y < s - r) return 1;
    cx = (x < r) ? r : s - 1 - r;
    cy = (y < r) ? r : s - 1 - r;
    return ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r);
}

static double SegDist(double px, double py, double x1, double y1, double x2, double y2)
{
    double dx = x2 - x1, dy = y2 - y1;
    double l2 = dx * dx + dy * dy, t, ex, ey;

    if (l2 <= 0.0001) { ex = px - x1; ey = py - y1; }
    else {
        t = ((px - x1) * dx + (py - y1) * dy) / l2;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        ex = px - (x1 + t * dx);
        ey = py - (y1 + t * dy);
    }
    return sqrt(ex * ex + ey * ey);
}

static unsigned char *Render(int s)
{
    static const double check[3][2] = { { 9.5, 16.5 }, { 13.5, 20.5 }, { 22.5, 10.5 } };
    unsigned char *px;
    double scale = (double)s / 32.0;
    double stroke = (s <= 16) ? 1.6 : 2.4 * scale + 0.5;
    int x, y, r;

    px = (unsigned char *)calloc((size_t)s * s * 4, 1);
    if (!px) return NULL;
    r = (s <= 16) ? 3 : (s <= 32 ? 5 : s / 5);

    for (y = 0; y < s; y++) {
        for (x = 0; x < s; x++) {
            unsigned char *p = px + ((size_t)y * s + x) * 4;
            double d1, d2, d, k, alpha;

            if (!InRounded(x, y, s, r)) { p[0] = p[1] = p[2] = p[3] = 0; continue; }

            d1 = SegDist(x + 0.5, y + 0.5, check[0][0] * scale, check[0][1] * scale,
                         check[1][0] * scale, check[1][1] * scale);
            d2 = SegDist(x + 0.5, y + 0.5, check[1][0] * scale, check[1][1] * scale,
                         check[2][0] * scale, check[2][1] * scale);
            d = (d1 < d2) ? d1 : d2;

            alpha = stroke - d + 0.5;
            if (alpha < 0) alpha = 0;
            if (alpha > 1) alpha = 1;

            k = 1.0 - 0.28 * (double)y / (double)(s > 1 ? s - 1 : 1);
            /* base #22C55E, BGRA order */
            p[0] = (unsigned char)(0x5E * k);
            p[1] = (unsigned char)(0xC5 * k);
            p[2] = (unsigned char)(0x22 * k);
            if (alpha > 0) {
                p[0] = (unsigned char)(p[0] * (1.0 - alpha) + 255.0 * alpha);
                p[1] = (unsigned char)(p[1] * (1.0 - alpha) + 255.0 * alpha);
                p[2] = (unsigned char)(p[2] * (1.0 - alpha) + 255.0 * alpha);
            }
            p[3] = 255;
        }
    }
    return px;
}

static long WriteImage(FILE *f, int s)
{
    BITMAPINFOHEADER bi;
    unsigned char *px = Render(s);
    unsigned char zero = 0;
    int rowBytes, y;
    size_t maskBytes, i;
    long start;

    if (!px) return 0;
    start = ftell(f);
    rowBytes = s * 4;

    ZeroMemory(&bi, sizeof(bi));
    bi.biSize = sizeof(bi);
    bi.biWidth = s;
    bi.biHeight = s * 2;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = 0;
    bi.biSizeImage = (DWORD)(rowBytes * s);
    fwrite(&bi, sizeof(bi), 1, f);

    for (y = s - 1; y >= 0; y--)
        fwrite(px + (size_t)y * rowBytes, (size_t)rowBytes, 1, f);

    maskBytes = (size_t)(((s + 31) / 32) * 4) * (size_t)s;
    for (i = 0; i < maskBytes; i++)
        fwrite(&zero, 1, 1, f);

    free(px);
    return ftell(f) - start;
}

int main(int argc, char **argv)
{
    static const int sizes[7] = { 16, 24, 32, 48, 64, 128, 256 };
    int n = 7, i;
    const char *path = (argc > 1) ? argv[1] : "AutoClip.ico";
    FILE *f;
    long offset;
    long dataStart[7];
    long dataLen[7];

    f = fopen(path, "wb");
    if (!f) { puts("cannot open output"); return 1; }

    fputc(0, f); fputc(0, f); fputc(1, f); fputc(0, f);
    fputc(n & 0xFF, f); fputc(0, f);

    /* reserve 16 bytes per entry */
    offset = 6 + 16 * n;
    for (i = 0; i < n; i++) {
        unsigned char e[16];
        int k;
        for (k = 0; k < 16; k++) e[k] = 0;
        fwrite(e, 16, 1, f);
    }

    for (i = 0; i < n; i++) {
        dataStart[i] = ftell(f);
        dataLen[i] = WriteImage(f, sizes[i]);
    }

    for (i = 0; i < n; i++) {
        unsigned char e[16];
        int k;
        for (k = 0; k < 16; k++) e[k] = 0;
        e[0] = (unsigned char)((sizes[i] >= 256) ? 0 : sizes[i]);
        e[1] = e[0];
        e[2] = 0;                       /* palette */
        e[3] = 0;
        e[4] = 1; e[5] = 0;             /* planes */
        e[6] = 32; e[7] = 0;            /* bpp */
        e[8]  = (unsigned char)(dataLen[i] & 0xFF);
        e[9]  = (unsigned char)((dataLen[i] >> 8) & 0xFF);
        e[10] = (unsigned char)((dataLen[i] >> 16) & 0xFF);
        e[11] = (unsigned char)((dataLen[i] >> 24) & 0xFF);
        e[12] = (unsigned char)(dataStart[i] & 0xFF);
        e[13] = (unsigned char)((dataStart[i] >> 8) & 0xFF);
        e[14] = (unsigned char)((dataStart[i] >> 16) & 0xFF);
        e[15] = (unsigned char)((dataStart[i] >> 24) & 0xFF);
        fseek(f, 6 + 16 * i, SEEK_SET);
        fwrite(e, 16, 1, f);
    }

    fclose(f);

    {
        FILE *log = fopen("icon_report.txt", "w");
        if (log) {
            fprintf(log, "entries=%d\n", n);
            for (i = 0; i < n; i++)
                fprintf(log, "%d px: offset=%ld bytes=%ld\n", sizes[i], dataStart[i], dataLen[i]);
            fclose(log);
        }
    }
    puts("icon written");
    return 0;
}
