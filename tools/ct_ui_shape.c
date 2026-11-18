// ct_ui_shape TEXT : CoreText glyphs + absolute positions for TEXT in the
// system UI font at size = the run font's upem, from its own fallback.
// Prints "PSNAME: gid@x,y ... |width" per run.
#include <CoreText/CoreText.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
    // 1st pass at 13 pt to learn the run fonts' upem
    double size = argc > 2 ? atof(argv[2]) : 1000;
    CTFontRef ui = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, NULL);
    CFStringRef keys[] = {kCTFontAttributeName}; CFTypeRef vals[] = {ui};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void**)keys, (const void**)vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CTLineRef line = CTLineCreateWithAttributedString(CFAttributedStringCreate(NULL, CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8), attrs));
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    for (CFIndex r = 0; r < CFArrayGetCount(runs); ++r) {
        CTRunRef run = CFArrayGetValueAtIndex(runs, r);
        CTFontRef rf = CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
        CFStringRef nm = CTFontCopyPostScriptName(rf); char b[256]; CFStringGetCString(nm, b, 256, kCFStringEncodingUTF8);
        const double scale = CTFontGetUnitsPerEm(rf) / CTFontGetSize(rf);
        CFIndex n = CTRunGetGlyphCount(run); CGPoint p[512]; CGGlyph g[512];
        CTRunGetPositions(run, CFRangeMake(0, 0), p); CTRunGetGlyphs(run, CFRangeMake(0, 0), g);
        printf("%s:", b);
        for (CFIndex i = 0; i < n; ++i) printf(" %d@%.0f,%.0f", g[i], (p[i].x - p[0].x) * scale, p[i].y * scale);
        printf("\n");
    }
}
