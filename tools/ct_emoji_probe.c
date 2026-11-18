#include <CoreText/CoreText.h>
#include <stdio.h>
int main() {
    for (double s = 8; s <= 160; s += (s < 40 ? 1 : 8)) {
        CTFontRef p = CTFontCreateWithName(CFSTR("AppleColorEmoji"), s, NULL);
        CTFontRef ui = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, s, NULL);
        CFStringRef keys[] = {kCTFontAttributeName}; CFTypeRef vals[] = {ui};
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void**)keys, (const void**)vals, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CTLineRef line = CTLineCreateWithAttributedString(CFAttributedStringCreate(NULL, CFSTR("\U0001F600"), attrs));
        CTRunRef run = CFArrayGetValueAtIndex(CTLineGetGlyphRuns(line), 0);
        CTFontRef rf = CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
        CGGlyph g; CTRunGetGlyphs(run, CFRangeMake(0, 1), &g);
        CGSize ap, au; CTFontGetAdvancesForGlyphs(p, 0, &g, &ap, 1); CTFontGetAdvancesForGlyphs(rf, 0, &g, &au, 1);
        CGRect rp = CTFontGetBoundingRectsForGlyphs(p, 0, &g, NULL, 1), ru = CTFontGetBoundingRectsForGlyphs(rf, 0, &g, NULL, 1);
        CGAffineTransform m = CTFontGetMatrix(rf);
        printf("%4.0f plain adv %6.3f img x %6.3f w %6.3f y %7.3f h %6.3f | ui adv %6.3f img x %6.3f w %6.3f y %7.3f h %6.3f  ui matrix a=%.4f size %.2f\n",
               s, ap.width, rp.origin.x, rp.size.width, rp.origin.y, rp.size.height,
               au.width, ru.origin.x, ru.size.width, ru.origin.y, ru.size.height, m.a, CTFontGetSize(rf));
    }
}
