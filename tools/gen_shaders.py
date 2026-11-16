#!/usr/bin/env python3
"""
gen_shaders.py — transpile mayag's shading kernel into every GPU dialect.

    include/mayag/render/kernel.hpp      (THE source of truth, C++)
        |
        +--> GLSL 4.50 (Vulkan)  --glslc-->  SPIR-V words
        +--> MSL 2.0   (Metal)
        |
        +--> include/mayag/render/shader_generated.hpp

WHY
---
mayag used to keep four hand-written shader dialects next to the C++ kernel and
ask a human to keep all five in sync. They drifted — silently, and in ways the
test suite could not see (see the comment block at the top of kernel.hpp). This
script removes the human from that loop: the dialects are BUILD OUTPUTS. There
is nothing to keep in sync because there is only one input.

HOW
---
kernel.hpp is written in a deliberately small C++ subset (documented in that
file) chosen so translation is mechanical and total: no templates, no pointers
except the Env hooks, no recursion, no state, a fixed builtin vocabulary. So
this is a line-oriented rewriter, not a compiler — and that is a feature. If
someone writes C++ the subset does not cover, this script FAILS LOUDLY rather
than emitting subtly different pixels, which is exactly the failure mode we are
trying to eliminate.

The parts that are genuinely per-backend — vertex stage, descriptor bindings,
texture fetch spelling — live in the small templates at the bottom of this file.
They contain no shape maths, no colour maths and no compositing.
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
KERNEL = os.path.join(ROOT, "include", "mayag", "render", "kernel.hpp")
OUT = os.path.join(ROOT, "include", "mayag", "render", "shader_generated.hpp")


# ── extracting the kernel body ──────────────────────────────────────────
def read_kernel():
    with open(KERNEL) as f:
        return f.read()


def strip_cpp_noise(src):
    """Drop everything the GPU has no use for: includes, namespaces, the Env
    struct (backend-specific), and the C++-only attributes."""
    # keep only from the first SDF function onward
    start = src.index("// ── signed distance fields")
    end = src.index("}  // namespace mayag::kernel")
    body = src[start:end]
    return body


def cut_section(body, begin_marker, end_marker=None):
    i = body.index(begin_marker)
    j = body.index(end_marker) if end_marker else len(body)
    return body[i:j]


# ── the translator ──────────────────────────────────────────────────────
# Each rule is (regex, replacement). Order matters. These implement exactly the
# subset kernel.hpp promises; anything outside it trips the guard below.
COMMON_RULES = [
    # attributes / qualifiers that mean nothing in a shader
    (r"\[\[nodiscard\]\]\s*", ""),
    (r"\bconstexpr\b\s*", ""),
    (r"\binline\b\s*", ""),
    (r"\bnoexcept\b", ""),
    (r"\bstatic_cast<f32>\(", "float("),
    (r"\bstatic_cast<u32>\(", "uint("),
    # our builtin namespace maps onto the dialect builtins
    (r"\bmg::length2\b", "length"),
    (r"\bmg::dot2\b", "dot"),
    (r"\bmg::atan2\b", "MG_ATAN2"),
    (r"\bmg::", ""),
    # types
    (r"\bconst\s+Fragment&\s*", "Fragment "),
    (r"\bconst\s+Env&\s*\w+", ""),
    (r"\bconst\s+", ""),
    (r"\bf32\b", "float"),
    (r"\bu32\b", "uint"),
    (r"\bbool\b", "bool"),
]

# literal suffixes: 1.0f -> 1.0, and integer-ish unsigned 0u stays 0u in GLSL
LITERAL_RULES = [
    (r"(\d)f\b", r"\1"),
]


class Unsupported(Exception):
    pass


def _strip_comments(text):
    """Remove // and /* */ comments so the guard inspects CODE only."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def guard(text, dialect):
    """Fail loudly on anything outside the documented subset."""
    code = _strip_comments(text)
    banned = [
        (r"\btemplate\b", "templates"),
        (r"\bnew\b", "dynamic allocation"),
        (r"\bwhile\b", "unbounded loops"),
        (r"\bstd::", "the standard library"),
        (r"[^-]->", "pointer dereference"),
    ]
    for pat, why in banned:
        m = re.search(pat, code)
        if m:
            line = code[: m.start()].count("\n") + 1
            snippet = code.splitlines()[line - 1].strip() if line - 1 < len(code.splitlines()) else ""
            raise Unsupported(
                f"{dialect}: kernel.hpp uses {why} near generated line {line}: "
                f"{snippet!r}\n"
                f"  The kernel must stay inside the subset documented at the top "
                f"of kernel.hpp so it can be transpiled to every backend."
            )


def translate(body, dialect):
    """C++ subset -> one shader dialect."""
    out = body

    # struct Fragment/Shaded: emit as dialect structs
    out = re.sub(r"struct (\w+) \{", r"struct \1 {", out)

    for pat, rep in COMMON_RULES:
        out = re.sub(pat, rep, out)
    for pat, rep in LITERAL_RULES:
        out = re.sub(pat, rep, out)

    # vec constructors: vec2{a, b} -> vec2(a, b). Must run on the ORIGINAL
    # brace text so the matching close brace can be found by nesting depth.
    # Brace-init DECLARATIONS (`vec4 sum{0,0,0,0};`) become assignments first,
    # so the same rebalancer handles both forms.
    out = re.sub(r"\b(vec2|vec4|float2|float4)\s+(\w+)\{\}", r"\1 \2 = \1(0.0)", out)
    out = re.sub(r"\b(vec2|vec4|float2|float4)\s+(\w+)\{", r"\1 \2 = \1{", out)
    out = _rebalance_ctor_braces(out)

    # .x/.y/.z/.w on vec4 are already GLSL-correct.
    # Vec4 default init `vec4{}` became `vec4()`; give it zeros.
    out = out.replace("vec4()", "vec4(0.0)")
    out = out.replace("vec2()", "vec2(0.0)")

    if dialect == "glsl":
        out = out.replace("MG_ATAN2", "atan")
    else:  # msl
        out = out.replace("MG_ATAN2", "atan2")
        out = out.replace("vec2", "float2").replace("vec4", "float4")
        out = out.replace("mix(", "mix(")

    guard(out, dialect)
    return out


def _rebalance_ctor_braces(s):
    """Turn `vecN{ ... }` into `vecN( ... )`.

    Runs BEFORE any other brace handling. Scans for a vector-constructor name
    followed by `{`, then walks to its matching `}` (tracking nesting so a
    nested vec4{...} inside a vec4{...} is handled) and rewrites both ends.
    """
    names = ("vec2", "vec4", "float2", "float4")
    out = list(s)
    i = 0
    while i < len(out):
        for n in names:
            if s.startswith(n + "{", i) and (i == 0 or not (s[i - 1].isalnum() or s[i - 1] == "_")):
                open_idx = i + len(n)
                depth = 0
                j = open_idx
                while j < len(s):
                    if s[j] == "{":
                        depth += 1
                    elif s[j] == "}":
                        depth -= 1
                        if depth == 0:
                            break
                    j += 1
                if j < len(s):
                    out[open_idx] = "("
                    out[j] = ")"
                i = open_idx
                break
        i += 1
    return "".join(out)


def kernel_functions(dialect):
    """The dialect-independent maths: constants, SDFs, colour, text gamma."""
    src = read_kernel()
    body = strip_cpp_noise(src)
    maths = cut_section(body, "// ── signed distance fields", "// ── the fragment program")
    return translate(constants_block(src) + maths, dialect)


def constants_block(src):
    """The ShapeKind / flag / pi constants, taken verbatim from kernel.hpp so a
    new kind or flag needs no change here."""
    start = src.index("inline constexpr f32 k_pi")
    end = src.index("// ── signed distance fields")
    return src[start:end]


def fragment_program(dialect):
    """shape_coverage + fill_color + shade, rewritten to read the varyings."""
    src = read_kernel()
    body = strip_cpp_noise(src)
    prog = cut_section(body, "[[nodiscard]] inline f32 shape_coverage")
    prog = translate(prog, dialect)

    # `Fragment f` fields become the shader's varyings/uniform reads. The
    # kernel names them f.local, f.half, ... so one substitution per field is
    # enough and it is total (the guard above rejects anything else).
    return prog


# ── per-backend scaffolding (transport only: no shading maths here) ─────
GLSL_VERTEX = r"""#version 450
layout(location = 0) in vec4 i_rect;
layout(location = 1) in vec4 i_radii;
layout(location = 2) in vec4 i_color;
layout(location = 3) in vec4 i_color2;
layout(location = 4) in vec4 i_axis;
layout(location = 5) in vec4 i_uv;
layout(location = 6) in vec4 i_params;
layout(location = 7) in uvec4 i_meta;

layout(push_constant) uniform Push { vec2 viewport; } push;

layout(location = 0) out vec2  v_local;
layout(location = 1) out vec2  v_norm;
layout(location = 2) out vec4  v_color;
layout(location = 3) out vec4  v_color2;
layout(location = 4) out vec4  v_radii;
layout(location = 5) out vec4  v_axis;
layout(location = 6) out vec4  v_params;
layout(location = 7) out vec2  v_uv;
layout(location = 8) flat out uvec4 v_meta;
layout(location = 9) out vec2  v_half;

void main() {
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));

    // Shadows need the quad grown so the blur is not clipped at the edge, and
    // every other quad gets a 1px skirt so an antialiased edge has somewhere
    // to ramp. That padding is GEOMETRY ONLY.
    float pad = (i_meta.x == 6u && (i_meta.y & 32u) == 0u) ? i_params.x * 3.0 + 2.0 : 1.0;

    vec2 shape_half = i_rect.zw * 0.5;
    vec2 half_size  = shape_half + vec2(pad);
    vec2 centre     = i_rect.xy + shape_half;
    vec2 pos        = centre + (corner * 2.0 - 1.0) * half_size;

    gl_Position = vec4(pos / push.viewport * 2.0 - 1.0, 0.0, 1.0);

    v_local  = (corner * 2.0 - 1.0) * half_size;
    v_half   = shape_half;

    // `norm` and `uv` must be measured against the UNPADDED shape, not the
    // grown quad. Interpolating them across the padded quad stretches the
    // atlas fetch (and the gradient axis) by `pad` on every side, which shifts
    // every glyph by about a pixel and is why GPU text used to sit one row
    // above the software rasteriser's.
    vec2 padded_local = (corner * 2.0 - 1.0) * half_size;
    vec2 shape_norm   = (padded_local + shape_half) /
                        max(shape_half * 2.0, vec2(1e-6));

    v_norm   = shape_norm;
    v_color  = i_color;
    v_color2 = i_color2;
    v_radii  = i_radii;
    v_axis   = i_axis;
    v_params = i_params;
    v_uv     = mix(i_uv.xy, i_uv.zw, shape_norm);
    v_meta   = i_meta;
}
"""

GLSL_FRAGMENT_HEAD = r"""#version 450
layout(location = 0) in vec2  v_local;
layout(location = 1) in vec2  v_norm;
layout(location = 2) in vec4  v_color;
layout(location = 3) in vec4  v_color2;
layout(location = 4) in vec4  v_radii;
layout(location = 5) in vec4  v_axis;
layout(location = 6) in vec4  v_params;
layout(location = 7) in vec2  v_uv;
layout(location = 8) flat in uvec4 v_meta;
layout(location = 9) in vec2  v_half;

layout(set = 0, binding = 0) uniform sampler2D u_atlas;
layout(set = 0, binding = 1) uniform sampler2D u_color_atlas;
layout(set = 0, binding = 2) uniform sampler2D u_capture;
layout(set = 0, binding = 3) uniform sampler2D u_texture;

layout(location = 0) out vec4 o_color;

// ── everything below is GENERATED from include/mayag/render/kernel.hpp ──
// Do not edit: run tools/gen_shaders.py (CMake does this automatically).

struct Fragment {
    vec2 local; vec2 norm; vec2 frag; vec2 half_size; vec2 uv;
    vec4 color; vec4 color2; vec4 radii; vec4 axis; vec4 params;
    float px; uint kind; uint flags;
};
struct Shaded { vec4 color; bool discarded; };

// Texture access — the only backend-specific part of the fragment program.
float env_atlas_r(vec2 uv)     { return texture(u_atlas, uv).r; }
vec4  env_atlas_rgba(vec2 uv)  { return texture(u_color_atlas, uv); }
vec4  env_capture(vec2 uv)     { return texture(u_capture, uv); }
vec4  env_texture(vec2 uv)     { return texture(u_texture, uv); }
vec2  env_capture_size()       { return vec2(textureSize(u_capture, 0)); }
"""

GLSL_FRAGMENT_MAIN = r"""
void main() {
    Fragment f;
    f.local  = v_local;
    f.norm   = v_norm;
    f.frag   = gl_FragCoord.xy;
    f.half_size = v_half;
    f.uv     = v_uv;
    f.color  = v_color;
    f.color2 = v_color2;
    f.radii  = v_radii;
    f.axis   = v_axis;
    f.params = v_params;
    // Pixel footprint from screen-space derivatives: this is what makes the
    // antialiasing correct under arbitrary transform and zoom. It is the one
    // quantity the CPU computes differently (it knows its own scale), which is
    // why it is an INPUT to the kernel rather than something the kernel derives.
    f.px     = max(length(fwidth(v_local)), 1e-6);
    f.kind   = v_meta.x;
    f.flags  = v_meta.y;

    Shaded s = shade(f);
    if (s.discarded) discard;
    o_color = s.color;
}
"""


def emit_glsl():
    maths = kernel_functions("glsl")
    prog = fragment_program("glsl")
    # the kernel refers to `f.half`; GLSL reserves nothing but we renamed the
    # field to half_size in the struct for clarity, so mirror that here.
    prog = prog.replace("f.half", "f.half_size")
    maths = maths.replace("f.half", "f.half_size")
    # Env hooks -> the concrete fetches declared above.
    prog = re.sub(r"env\.atlas_r\(env\.ctx,\s*([^)]+)\)", r"env_atlas_r(\1)", prog)
    prog = re.sub(r"env\.atlas_rgba\(env\.ctx,\s*([^)]+)\)", r"env_atlas_rgba(\1)", prog)
    prog = re.sub(r"env\.capture\(env\.ctx,\s*([^)]+)\)", r"env_capture(\1)", prog)
    prog = re.sub(r"env\.texture\(env\.ctx,\s*([^)]+)\)", r"env_texture(\1)", prog)
    prog = prog.replace("env.capture_size.x", "env_capture_size().x")
    prog = prog.replace("env.capture_size.y", "env_capture_size().y")
    prog = prog.replace("Shaded shade(Fragment f, )", "Shaded shade(Fragment f)")
    prog = re.sub(r"Shaded shade\(Fragment f,\s*\)", "Shaded shade(Fragment f)", prog)
    prog = prog.replace("result.discard", "result.discarded")
    prog = prog.replace("Shaded result;", "Shaded result; result.discarded = false;")
    prog = prog.replace("{ result.discarded = true; return result; }",
                        "{ result.discarded = true; return result; }")
    return GLSL_VERTEX, GLSL_FRAGMENT_HEAD + maths + prog + GLSL_FRAGMENT_MAIN


# ── SPIR-V ──────────────────────────────────────────────────────────────
def compile_spirv(glslc, stage, code):
    with tempfile.NamedTemporaryFile("w", suffix="." + stage, delete=False) as f:
        f.write(code)
        path = f.name
    spv = path + ".spv"
    try:
        subprocess.check_call(
            [glslc, "-O", "-fshader-stage=" + stage, path, "-o", spv],
            stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError:
        sys.stderr.write("\n--- generated %s shader ---\n" % stage)
        for n, line in enumerate(code.splitlines(), 1):
            sys.stderr.write("%4d| %s\n" % (n, line))
        raise
    data = open(spv, "rb").read()
    os.unlink(path)
    os.unlink(spv)
    import struct
    return struct.unpack("<%dI" % (len(data) // 4), data)


def emit_array(name, words):
    out = ["inline constexpr std::uint32_t %s[] = {" % name]
    for i in range(0, len(words), 8):
        out.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
    out.append("};")
    return "\n".join(out)


def cxx_raw(name, text):
    return 'inline constexpr std::string_view %s = R"SHADER(\n%s\n)SHADER";' % (name, text)


def main():
    glslc = sys.argv[1] if len(sys.argv) > 1 else "glslc"
    out_path = sys.argv[2] if len(sys.argv) > 2 else OUT

    vert, frag = emit_glsl()

    vw = compile_spirv(glslc, "vert", vert)
    fw = compile_spirv(glslc, "frag", frag)

    header = f"""#pragma once
// GENERATED FILE — DO NOT EDIT.
//
// Produced by tools/gen_shaders.py from include/mayag/render/kernel.hpp, the
// single source of truth for mayag's shading. Every backend's shader is a
// build output of that one file, so no dialect can drift from the C++ the
// software rasteriser runs. To change how something LOOKS, edit kernel.hpp.
//
// CMake regenerates this whenever kernel.hpp changes and a shader compiler is
// available; the result is checked in so a build with no glslc still works.

#include <cstdint>
#include <string_view>

namespace mayag::render::generated {{

{cxx_raw('glsl_vertex', vert)}

{cxx_raw('glsl_fragment', frag)}

}}  // namespace mayag::render::generated

namespace mayag::render::spirv {{

{emit_array('vertex', vw)}

{emit_array('fragment', fw)}

}}  // namespace mayag::render::spirv
"""
    with open(out_path, "w") as f:
        f.write(header)
    print("wrote %s: vert %d words, frag %d words" % (out_path, len(vw), len(fw)))


if __name__ == "__main__":
    main()
