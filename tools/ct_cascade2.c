// ct_cascade2 : read hex codepoints on stdin; for each print
// "cp<TAB>path<TAB>face_index<TAB>postscript" of the font CoreText's UI-font
// fallback picks (CTFontCreateForString).
#include <CoreText/CoreText.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char cache_ps[256][256]; static int cache_idx[256]; static char cache_path[256][1024]; static int ncache = 0;
static int face_index(CFURLRef url, CFStringRef ps, const char* path, const char* psc) {
    for (int i = 0; i < ncache; ++i) if (!strcmp(cache_ps[i], psc) && !strcmp(cache_path[i], path)) return cache_idx[i];
    int idx = 0;
    CFArrayRef ds = CTFontManagerCreateFontDescriptorsFromURL(url);
    if (ds) for (CFIndex k = 0; k < CFArrayGetCount(ds); ++k) {
        CFStringRef n = CTFontDescriptorCopyAttribute(CFArrayGetValueAtIndex(ds, k), kCTFontNameAttribute);
        if (n && CFStringCompare(n, ps, 0) == kCFCompareEqualTo) { idx = (int)k; break; }
    }
    if (ncache < 256) { strcpy(cache_ps[ncache], psc); strcpy(cache_path[ncache], path); cache_idx[ncache++] = idx; }
    return idx;
}
int main() {
    CTFontRef ui = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 13, NULL);
    char line[64];
    while (fgets(line, sizeof line, stdin)) {
        unsigned cp = (unsigned)strtoul(line, NULL, 16), c = cp;
        UniChar u[2]; CFIndex n = 1;
        if (c > 0xFFFF) { c -= 0x10000; u[0] = 0xD800 + (c >> 10); u[1] = 0xDC00 + (c & 0x3FF); n = 2; }
        else u[0] = (UniChar)c;
        CFStringRef s = CFStringCreateWithCharacters(NULL, u, n);
        CTFontRef f = CTFontCreateForString(ui, s, CFRangeMake(0, n));
        CFStringRef ps = CTFontCopyPostScriptName(f);
        CFURLRef url = CTFontCopyAttribute(f, kCTFontURLAttribute);
        char b[256], p[1024] = "";
        CFStringGetCString(ps, b, sizeof b, kCFStringEncodingUTF8);
        if (url) CFURLGetFileSystemRepresentation(url, true, (UInt8*)p, sizeof p);
        printf("%X\t%s\t%d\t%s\n", cp, p, url ? face_index(url, ps, p, b) : 0, b);
        CFRelease(s); CFRelease(f); CFRelease(ps); if (url) CFRelease(url);
    }
}
