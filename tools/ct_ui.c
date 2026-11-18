// ct_ui out.png "text" px : CoreText render in the system UI font (with its
// own fallback), same layout/colours as mg_px '-'.
#include <ApplicationServices/ApplicationServices.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv) {
    double px = atof(argv[3]);
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, px, NULL);
    int W = 900, H = (int)(px * 2.2);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(NULL, W, H, 8, W * 4, cs, kCGImageAlphaPremultipliedLast);
    CGContextSetRGBFillColor(ctx, 0.98, 0.98, 0.98, 1);
    CGContextFillRect(ctx, CGRectMake(0, 0, W, H));
    CGColorRef ink = CGColorCreateSRGB(0x1C / 255.0, 0x1F / 255.0, 0x24 / 255.0, 1);
    CFStringRef keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    CFTypeRef vals[] = {font, ink};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void**)keys, (const void**)vals, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8), attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CGFloat asc = CTFontGetAscent(font);
    CGContextSetTextPosition(ctx, 4.0, H - px * 0.35 - asc);
    CTLineDraw(line, ctx);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef ou = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)argv[1], strlen(argv[1]), false);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(ou, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(d, img, NULL);
    CGImageDestinationFinalize(d);
    printf("baseline_from_top %.1f\n", px * 0.35 + asc);
    return 0;
}
