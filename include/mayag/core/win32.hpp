#pragma once
#include "target.hpp"
// mayag/core/win32.hpp — the ONE way mayag's headers include <windows.h>.
//
// mayag is header-only, so whatever <windows.h> defines lands in every
// translation unit of every program that uses mayag. Left alone it defines,
// among a few thousand other things:
//
//   min / max           breaks std::min, std::max, numeric_limits<T>::max()
//   near / far          16-bit memory-model keywords, defined to NOTHING, so
//                       `bool near = ...` or `bool near(float, float)` stops
//                       compiling (hit in hand's input code and in mayag's
//                       own colour and SDF headers)
//   IN / OUT / OPTIONAL SAL-era annotations, also empty: `LineBreak::IN` in
//                       the font engine vanished
//
// So every include goes through here: lean, NOMINMAX, and the empty
// keyword macros removed again afterwards. Nothing in the Windows SDK
// headers needs them once those headers have been parsed.

#if MAYAG_OS_WINDOWS
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX 1
#  endif
#  include <windows.h>
#  undef near
#  undef far
#  undef IN
#  undef OUT
#  undef OPTIONAL
// NOT `interface`: it is not empty but `struct`, and every COM header
// parsed after this one (combaseapi.h, ole2.h, d3d11.h, dxgi, dwrite_2.h,
// dwmapi.h) spells `typedef interface ID3D11Device ID3D11Device;`.
// Undefining it broke every MSVC build that reached a COM header (85
// errors a file, from combaseapi.h on). Nothing in mayag or hand names it.
#endif
