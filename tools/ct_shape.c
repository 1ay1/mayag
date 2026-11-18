// ct_shape FONT INDEX TEXT [notrack] : CoreText's glyphs + absolute positions
// at size = unitsPerEm (so units are font units). Prints visual order:
//   gid@x,y gid@x,y ... |width
#include <ApplicationServices/ApplicationServices.h>
#include <CoreText/CoreText.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    CFStringRef path = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, false);
    CFArrayRef descs = CTFontManagerCreateFontDescriptorsFromURL(url);
    const long idx = atol(argv[2]);
    if (!descs || CFArrayGetCount(descs) <= idx) { fprintf(stderr, "no face\n"); return 1; }
    CTFontDescriptorRef d = CFArrayGetValueAtIndex(descs, idx);
    CTFontRef f1 = CTFontCreateWithFontDescriptor(d, 12.0, NULL);
    const double upem = CTFontGetUnitsPerEm(f1);
    CTFontRef font = CTFontCreateWithFontDescriptor(d, upem, NULL);
    // A variable single-face file lists its NAMED instances; index 0 may be
    // "Black". Open the file itself for the default instance.
    if (CFArrayGetCount(descs) > 1 && strstr(argv[1], ".ttc") == NULL) {
        CGDataProviderRef dp = CGDataProviderCreateWithURL(url);
        CGFontRef cg = CGFontCreateWithDataProvider(dp);
        font = CTFontCreateWithGraphicsFont(cg, upem, NULL, NULL);
    }
    CFStringRef str = CFStringCreateWithCString(NULL, argv[3], kCFStringEncodingUTF8);
    if (argc > 4) {   // TYPE:SELECTOR -> force an AAT feature setting
        int ty = 0, sel = 0;
        sscanf(argv[4], "%d:%d", &ty, &sel);
        CFNumberRef t = CFNumberCreate(NULL, kCFNumberIntType, &ty), s = CFNumberCreate(NULL, kCFNumberIntType, &sel);
        const void* k[] = {kCTFontFeatureTypeIdentifierKey, kCTFontFeatureSelectorIdentifierKey};
        const void* v[] = {t, s};
        CFDictionaryRef setting = CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFArrayRef settings = CFArrayCreate(NULL, (const void**)&setting, 1, &kCFTypeArrayCallBacks);
        const void* ak[] = {kCTFontFeatureSettingsAttribute};
        const void* av[] = {settings};
        CFDictionaryRef fa = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CTFontDescriptorRef fd = CTFontDescriptorCreateWithAttributes(fa);
        font = CTFontCreateCopyWithAttributes(font, upem, NULL, fd);
    }
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCTFontAttributeName, font);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attrs);
    CTTypesetterRef ts = CTTypesetterCreateWithAttributedString(as);
    CTLineRef line = CTTypesetterCreateLine(ts, CFRangeMake(0, 0));
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    for (CFIndex r = 0; r < CFArrayGetCount(runs); ++r) {
        CTRunRef run = CFArrayGetValueAtIndex(runs, r);
        CFDictionaryRef ra = CTRunGetAttributes(run);
        CTFontRef rf = CFDictionaryGetValue(ra, kCTFontAttributeName);
        CFStringRef a = CTFontCopyPostScriptName(rf), b = CTFontCopyPostScriptName(font);
        if (CFStringCompare(a, b, 0) != 0) printf("[fallback] ");
        CFIndex n = CTRunGetGlyphCount(run);
        CGPoint* p = malloc(sizeof(CGPoint) * n);
        CGGlyph* g = malloc(sizeof(CGGlyph) * n);
        CTRunGetPositions(run, CFRangeMake(0, 0), p);
        CTRunGetGlyphs(run, CFRangeMake(0, 0), g);
        for (CFIndex i = 0; i < n; ++i) printf("%d@%.0f,%.0f ", g[i], p[i].x, p[i].y);
    }
    printf("|%.0f\n", CTLineGetTypographicBounds(line, NULL, NULL, NULL));
    return 0;
}
