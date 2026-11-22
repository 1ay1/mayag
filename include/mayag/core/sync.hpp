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

#include <jaal/core/frozen.hpp>
#include <jaal/core/sendable.hpp>
#include <jaal/core/shared.hpp>
#include <jaal/kernel/cell.hpp>

namespace mayag {

template <class T>
using Cell = jaal::cell<T>;

template <class T>
using Frozen = jaal::shared<T>;

}  // namespace mayag

// mayag's geometry and colour need no claim: their fields are public, so jaal
// proves them Sendable and Frozen field by field. Checked here, at the door:
static_assert(jaal::Sendable<mayag::Vec2> && jaal::Frozen<mayag::Vec2>);
static_assert(jaal::Sendable<mayag::Rect> && jaal::Frozen<mayag::Rect>);
static_assert(jaal::Sendable<mayag::Color<mayag::Srgb>> && jaal::Frozen<mayag::Color<mayag::Srgb>>);
