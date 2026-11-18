// ct_cascade : for each codepoint arg (hex), the font CTFontCreateForString
// picks from the UI font, as "cp postscript-name path".
#include <CoreText/CoreText.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
    CTFontRef ui = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 13, NULL);
    for (int i = 1; i < argc; ++i) {
        unsigned cp = (unsigned)strtoul(argv[i], NULL, 16);
        UniChar u[2]; CFIndex n = 1;
        if (cp > 0xFFFF) { cp -= 0x10000; u[0] = 0xD800 + (cp >> 10); u[1] = 0xDC00 + (cp & 0x3FF); n = 2; cp += 0x10000; }
        else u[0] = (UniChar)cp;
        CFStringRef s = CFStringCreateWithCharacters(NULL, u, n);
        CTFontRef f = CTFontCreateForString(ui, s, CFRangeMake(0, n));
        CFStringRef nm = CTFontCopyPostScriptName(f);
        CFStringRef fam = CTFontCopyFamilyName(f);
        CFURLRef url = CTFontCopyAttribute(f, kCTFontURLAttribute);
        char b[256], fb[256], p[1024] = "";
        CFStringGetCString(nm, b, sizeof b, kCFStringEncodingUTF8);
        CFStringGetCString(fam, fb, sizeof fb, kCFStringEncodingUTF8);
        if (url) CFURLGetFileSystemRepresentation(url, true, (UInt8*)p, sizeof p);
        printf("%X\t%s\t%s\t%s\n", cp, b, fb, p);
    }
}
