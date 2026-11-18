// ct_marks FONT INDEX TEXT : per glyph: gid, pen x, y, advance, ink bbox
#include <CoreText/CoreText.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
    CFStringRef path = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, false);
    CFArrayRef descs = CTFontManagerCreateFontDescriptorsFromURL(url);
    CTFontDescriptorRef d = CFArrayGetValueAtIndex(descs, atol(argv[2]));
    CTFontRef f1 = CTFontCreateWithFontDescriptor(d, 12.0, NULL);
    double sz = argc > 4 ? atof(argv[4]) : CTFontGetUnitsPerEm(f1); CTFontRef font = CTFontCreateWithFontDescriptor(d, sz, NULL);
    CFStringRef str = CFStringCreateWithCString(NULL, argv[3], kCFStringEncodingUTF8);
    CFStringRef keys[] = {kCTFontAttributeName}; CFTypeRef vals[] = {font};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void**)keys, (const void**)vals, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    for (CFIndex r = 0; r < CFArrayGetCount(runs); ++r) {
        CTRunRef run = CFArrayGetValueAtIndex(runs, r);
        CFIndex n = CTRunGetGlyphCount(run);
        CGPoint p[64]; CGGlyph g[64]; CGSize a[64];
        CTRunGetPositions(run, CFRangeMake(0, 0), p);
        CTRunGetGlyphs(run, CFRangeMake(0, 0), g);
        CTRunGetAdvances(run, CFRangeMake(0, 0), a);
        CTFontRef rf = CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
        for (CFIndex i = 0; i < n; ++i) {
            CGRect b = CTFontGetBoundingRectsForGlyphs(rf, kCTFontOrientationHorizontal, &g[i], NULL, 1);
            printf("%d %.2f %.2f adv %.1f ink %.0f %.0f %.0f %.0f\n", g[i], p[i].x, p[i].y, a[i].width,
                   b.origin.x, b.origin.y, b.origin.x + b.size.width, b.origin.y + b.size.height);
        }
    }
}
