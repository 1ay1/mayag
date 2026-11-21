#pragma once
// mayag/core/sync.hpp — mayag's thread-safety vocabulary, from jaal.
//
// The ONE door from mayag's lower layers (font, render, style, ...) to jaal's
// concurrency types. Everything below app/ that shares data between threads
// spells it with these names; nothing else below app/ includes jaal.
//
//   Cell<T>        a LEAF lock (jaal::cell): the body is captureless, its
//                  arguments and result are Sendable, and taking a second lock
//                  from inside it aborts on the first attempt. So caches
//                  behind an interface (a measurer, a glyph renderer) can
//                  neither race nor deadlock.
//   Frozen<T>      a value built once, then immutable and shared by any number
//                  of threads (jaal::shared): never null, only const access.
//
// Opt-ins are claims a human made; each one sits beside its type, with why:
//   MAYAG_FROZEN(T)     T is immutable once built
//   MAYAG_SENDABLE(T)   T owns everything it holds, so it may change threads
// (the macros are defined in app/sources.hpp for apps; lower layers write the
// specialisations of jaal's traits directly, beside the type, as below.)

#include "color.hpp"
#include "geometry.hpp"

#include <jaal/core/shared.hpp>
#include <jaal/kernel/cell.hpp>

namespace mayag {

template <class T>
using Cell = jaal::cell<T>;

template <class T>
using Frozen = jaal::shared<T>;

}  // namespace mayag

// mayag's geometry and colour are plain values (floats, no pointers), so they
// may cross threads and be shared frozen. jaal can't see inside a class with
// constructors, hence the claims -- here, beside the types, so every layer
// that includes this header sees them.
template <> inline constexpr bool jaal::sendable_opt_in<mayag::Vec2> = true;
template <> inline constexpr bool jaal::frozen_opt_in<mayag::Vec2>   = true;
template <> inline constexpr bool jaal::sendable_opt_in<mayag::Rect> = true;
template <> inline constexpr bool jaal::frozen_opt_in<mayag::Rect>   = true;
template <class S> inline constexpr bool jaal::sendable_opt_in<mayag::Color<S>> = true;
template <class S> inline constexpr bool jaal::frozen_opt_in<mayag::Color<S>>   = true;
