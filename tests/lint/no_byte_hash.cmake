# tests/lint/no_byte_hash.cmake — a key is made of VALUES, never of an
# object's bytes.
#
# A struct with padding (Theme: bools and one-byte enums between floats) has
# bytes that are whatever the stack or heap held. Hashing them makes a cache
# key change when nothing did: memo() missed at random -- a whole terminal
# re-laid on every keystroke -- and nothing failed, it was just slow. Equality
# and hashing go field by field (operator==, MemoKey::add, MemoCache::theme_id).
#
# So viewing an object's address as bytes (`reinterpret_cast<const unsigned
# char*>(&x)` and friends) is an error in these directories. Raw buffers --
# pixels, glyph bitmaps, file bytes -- are already byte pointers and don't
# take an address with `&`.

set(dirs app core dsl editor font image layout output render scene style widget)
set(bad 0)
foreach(d ${dirs})
    file(GLOB_RECURSE fs RELATIVE ${ROOT} ${ROOT}/${d}/*.hpp)
    foreach(rel ${fs})
        file(STRINGS ${ROOT}/${rel} hits
             REGEX "reinterpret_cast<const (unsigned char|char|std::uint8_t|uint8_t|std::byte) *\\*>\\(&")
        foreach(h ${hits})
            message(SEND_ERROR "${rel}: an object viewed as bytes (padding makes the bytes garbage; key on its fields):\n  ${h}")
            set(bad 1)
        endforeach()
    endforeach()
endforeach()
if(bad)
    message(FATAL_ERROR "no_byte_hash: key on values, not bytes")
endif()
