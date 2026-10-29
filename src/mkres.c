/*
 * mkres - embed an .ico into a PE file without a resource compiler.
 *
 * TCC ships no windres/rc, so the icon is added in two steps:
 *   1. append the .ico bytes to the executable
 *   2. append a new section whose IMAGE_RESOURCE_DIRECTORY advertises an icon
 *      (RT_ICON, id 101, language 1033), which is what LoadIconW(hInst,
 *      MAKEINTRESOURCE(101)) and Explorer look for
 *
 * usage: mkres <exe> <ico>
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *Slurp(const char *path, DWORD *outSize)
{
    FILE *f;
    unsigned char *buf;
    long n;

    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *outSize = (DWORD)n;
    return buf;
}

static DWORD Rd32(const unsigned char *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static unsigned Rd16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static void Wr16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void Wr32(unsigned char *p, DWORD v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

int main(int argc, char **argv)
{
    unsigned char *exe = NULL, *ico = NULL, *out = NULL;
    DWORD exeSize = 0, icoSize = 0;
    DWORD peOff, optOff, numSec, optSize, secTable, fileAlign, sectAlign;
    DWORD lastVA = 0, lastRaw = 0, newVA, rawSize, secFileOff, blobOff, total;
    DWORD rgneOff, typeOff, nameOff, langOff, dataBlobOff;
    FILE *f;

    if (argc < 3) { puts("usage: mkres <exe> <ico>"); return 2; }
    if (strlen(argv[1]) > 200 || strlen(argv[2]) > 200) { puts("path too long"); return 2; }

    exe = Slurp(argv[1], &exeSize);
    if (!exe) { puts("cannot read exe"); return 1; }
    ico = Slurp(argv[2], &icoSize);
    if (!ico) { puts("cannot read ico"); return 1; }

    if (exeSize < 0x200 || exe[0] != 'M' || exe[1] != 'Z') { puts("not a PE image"); return 1; }
    if (icoSize < 22) { puts("ico too small"); return 1; }

    peOff = Rd32(exe + 0x3C);
    if (peOff + 24 >= exeSize) { puts("bad PE header"); return 1; }
    optOff = peOff + 24;
    numSec = Rd16(exe + peOff + 6);
    optSize = Rd16(exe + peOff + 20);
    fileAlign = Rd32(exe + optOff + 36);
    sectAlign = Rd32(exe + optOff + 32);
    if (fileAlign == 0 || sectAlign == 0) { puts("bad alignment"); return 1; }
    secTable = optOff + optSize;
    if (secTable + numSec * 40 > exeSize) { puts("bad section table"); return 1; }

    {
        DWORD i;
        for (i = 0; i < numSec; i++) {
            unsigned char *s = exe + secTable + i * 40;
            DWORD vsize = Rd32(s + 8);
            DWORD vaddr = Rd32(s + 12);
            DWORD rsize = Rd32(s + 16);
            DWORD rptr = Rd32(s + 20);
            DWORD endVA = vaddr + (vsize > rsize ? vsize : rsize);
            DWORD endRaw = rptr + rsize;

            if (endVA > lastVA) lastVA = endVA;
            if (endRaw > lastRaw) lastRaw = endRaw;
        }
    }

    newVA = ((lastVA + sectAlign - 1) / sectAlign) * sectAlign;

    /* resource tree, section relative */
    rgneOff = 0;
    typeOff = rgneOff + 24;
    nameOff = typeOff + 24;
    langOff = nameOff + 24;
    dataBlobOff = langOff + 16 + 8;
    dataBlobOff = (dataBlobOff + 3) & ~3u;

    rawSize = ((dataBlobOff + icoSize + fileAlign - 1) / fileAlign) * fileAlign;
    secFileOff = ((lastRaw + fileAlign - 1) / fileAlign) * fileAlign;
    blobOff = secFileOff + rawSize;
    total = blobOff + icoSize + 40;

    out = (unsigned char *)calloc(total + 8, 1);
    if (!out) { puts("out of memory"); return 1; }
    memcpy(out, exe, exeSize);

    /* new section: name ".rsrc2" */
    {
        unsigned char *s = out + secTable + numSec * 40;
        Wr32(s, 0x3273722E);   /* leave the name mostly ASCII */
        Wr32(s + 4, 0x00000032);
        Wr32(s + 8, rawSize);
        Wr32(s + 12, newVA);
        Wr32(s + 16, rawSize);
        Wr32(s + 20, secFileOff);
        Wr32(s + 24, 0);
        Wr32(s + 28, 0);
        Wr16(s + 32, 0);
        Wr16(s + 34, 0);
        Wr32(s + 36, 0x40000040);
    }
    Wr16(out + peOff + 6, numSec + 1);
    Wr32(out + optOff + 56, ((newVA + rawSize + sectAlign - 1) / sectAlign) * sectAlign);

    /* three IMAGE_RESOURCE_DIRECTORY levels, one leaf */
    {
        unsigned char *d = out + secFileOff + rgneOff;
        Wr32(d + 12, 0);
        Wr32(d + 14, 1);
        Wr32(d + 16, 3);                        /* RT_ICON */
        Wr32(d + 20, 0x80000000u | typeOff);
    }
    {
        unsigned char *d = out + secFileOff + typeOff;
        Wr32(d + 12, 0);
        Wr32(d + 14, 1);
        Wr32(d + 16, 101);
        Wr32(d + 20, 0x80000000u | nameOff);
    }
    {
        unsigned char *d = out + secFileOff + nameOff;
        Wr32(d + 12, 0);
        Wr32(d + 14, 1);
        Wr32(d + 16, 1033);
        Wr32(d + 20, 0x80000000u | langOff);
    }
    {
        unsigned char *d = out + secFileOff + langOff;
        Wr32(d + 12, 0);
        Wr32(d + 14, 1);
        Wr32(d + 16, dataBlobOff);
        Wr32(d + 20, icoSize);
        Wr32(d + 24, 0);
        Wr32(d + 28, 0);
    }

    memcpy(out + secFileOff + dataBlobOff, ico, icoSize);

    /* trailing copy so other tools can recover the icon */
    memcpy(out + blobOff, ico, icoSize);

    {
        DWORD start = *(DWORD *)out;
        f = fopen(argv[1], "wb");
        (void)start;
        if (!f) { puts("cannot write exe"); return 1; }
        if (fwrite(out, 1, blobOff + icoSize, f) != blobOff + icoSize) {
            puts("short write");
            fclose(f);
            return 1;
        }
        fclose(f);
    }

    printf("icon embedded: section RVA 0x%lx size 0x%lx, icon RVA 0x%lx (%lu bytes)\n",
           (unsigned long)newVA, (unsigned long)rawSize,
           (unsigned long)(newVA + dataBlobOff), (unsigned long)icoSize);

    free(out);
    free(exe);
    free(ico);
    return 0;
}
