// ct_phase: draw "l" (SF, size px) at x = 10 + k/64, measure the ink centroid.
#include <ApplicationServices/ApplicationServices.h>
#include <CoreText/CoreText.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
    double px = atof(argv[1]);
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, px, NULL);
    UniChar ch = 'l'; CGGlyph g; CTFontGetGlyphsForCharacters(font, &ch, &g, 1);
    for (int k = 0; k < 96; k += 1) {
        int W = 64, H = 64;
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
        CGContextRef ctx = CGBitmapContextCreate(NULL, W, H, 8, W, cs, kCGImageAlphaNone);
        CGContextSetGrayFillColor(ctx, 1, 1); CGContextFillRect(ctx, CGRectMake(0, 0, W, H));
        CGContextSetGrayFillColor(ctx, 0, 1);
        CGPoint p = CGPointMake(10 + k / 96.0, 20);
        CTFontDrawGlyphs(font, &g, &p, 1, ctx);
        unsigned char* d = CGBitmapContextGetData(ctx);
        double s = 0, sx = 0;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { double c = (255 - d[y * W + x]) / 255.0; s += c; sx += c * (x + 0.5); }
        printf("%.4f %.4f\n", k / 96.0, sx / s - 10 - k / 96.0);
        CGContextRelease(ctx);
    }
}
