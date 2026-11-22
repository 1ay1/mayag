// tests/claims_audited.cpp — every trust claim in mayag, checked field by field.
//
// A claim (a Sendable / Frozen opt-in) is a human saying "jaal
// can't see inside this type, but it is safe". On a compiler with C++26
// reflection (GCC 16, -freflection) jaal CAN see inside -- private fields
// and all, recursively -- so each claim is checked here as a statement about
// LEAVES: every leaf is proven by jaal, or vouched for below by name or type
// family, with the reason. A new field nobody vouched for fails this build;
// a vouch that no longer matches anything fails it too.
//
// The test passes by COMPILING (static_asserts); running it only prints.
// Without reflection the audits are skipped (the claims stand as written).

#include <mayag/mayag.hpp>
#include <mayag/widget/sheet.hpp>

#include <cstdio>

using namespace mayag;
using mayag::audit::claim;
using mayag::audit::fields_t;
using mayag::audit::rule;
using mayag::audit::types_t;

// ── fonts ──────────────────────────────────────────────────────────────────

// The one OS mapping: ptr_ is the read-only mapping (PROT_READ /
// PAGE_READONLY) or owned_'s buffer, owned and freed only by this object.
static_assert(claim<typo::MappedFile, rule::shareable, fields_t<"ptr_">>);

// A face: its bytes are Frozen (owned or a MappedFile), its caches are Cells
// and jaal::lazy tables. The only unproven leaves are std::span views into
// its OWN frozen bytes (file_, the glyph sources, the tables), which live as
// long as the face because the face co-owns them.
static_assert(claim<typo::Face, rule::shareable, fields_t<>, types_t<"std::span">>);

// A compiled shaping plan (detail::FrozenPlan holds one, frozen):
//   shaper        a pointer to one of the static ScriptShaper tables
//   data          the shaper's private plan data, built with the plan, then read-only
//   "(*)("        function pointers: code, immutable (the normaliser, stage hooks)
//   std::span     views into the face's GSUB/GPOS/morx tables (the face outlives its plans)
static_assert(claim<typo::ot::ShapePlan, rule::shareable, fields_t<"shaper", "data">, types_t<"(*)(", "std::span">>);

// ── looks (process-wide sheets) ────────────────────────────────────────────

// A Look's Style and Cascade, built whole by the sheet builders:
//   scroll        a ScrollState the style follows -- sheet looks never set it
//                 (they are built from theme knobs, with no model state)
//   data, fn      type-erased pointers into memory owned by the same
//                 Cascade's `keep`
//   keep          those owners (shared_ptr<const void>): const, kept for life
//   sheet         Theme::sheet: a process-wide Sheet (sheets::tuned keeps every
//                 one for the life of the process, frozen), so the address is stable
//   "(*)("        function pointers: code
static_assert(claim<Look, rule::shareable, fields_t<"scroll", "data", "fn", "keep", "sheet">, types_t<"(*)(">>);

int main() {
#if defined(__cpp_impl_reflection)
    std::printf("claims audited field by field: MappedFile, Face, ShapePlan, Look\n");
#else
    std::printf("SKIP  no C++26 reflection: claims not audited on this compiler\n");
#endif
    return 0;
}
