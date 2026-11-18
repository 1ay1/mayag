// ct_fonts "text" px : the font CoreText's UI-font fallback picks per run.
#include <CoreText/CoreText.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, atof(argv[2]), NULL);
    CFStringRef keys[] = {kCTFontAttributeName}; CFTypeRef vals[] = {font};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void**)keys, (const void**)vals, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CTLineRef line = CTLineCreateWithAttributedString(
        CFAttributedStringCreate(NULL, CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8), attrs));
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    for (CFIndex r = 0; r < CFArrayGetCount(runs); ++r) {
        CTRunRef run = CFArrayGetValueAtIndex(runs, r);
        CTFontRef rf = CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
        CFStringRef nm = CTFontCopyPostScriptName(rf);
        CFURLRef url = CTFontCopyAttribute(rf, kCTFontURLAttribute);
        char b[256], u[512] = "";
        CFStringGetCString(nm, b, sizeof b, kCFStringEncodingUTF8);
        if (url) CFStringGetCString(CFURLGetString(url), u, sizeof u, kCFStringEncodingUTF8);
        CFRange cr = CTRunGetStringRange(run);
        printf("[%ld,%ld) %s %s\n", (long)cr.location, (long)(cr.location + cr.length), b, u);
    }
}
