#pragma once
// mayag::editor syntax — fast, incremental, table-driven highlighting
//
// A code editor highlights on every keystroke, so this is a single-pass lexer
// per line with a tiny carried state (am I inside a block comment, or a
// string that runs across lines?). That state is cached per line, so an edit
// on line 900 relexes from line 900 down to the first line whose state comes
// out unchanged, and never touches the 899 lines above it.
//
// It is NOT a parser. Tree-sitter-grade structure (scopes, real symbol
// kinds) is a different tool; this is the layer that makes code readable at
// once and costs microseconds per line. Heuristics on top of the lexer do
// most of what readers actually notice: `name(` is a function, `.name` is a
// property, `Name` is a type, `NAME` is a constant.

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mayag::editor {

/// What a run of characters is. The colour of each is the theme's business.
enum class Tok : std::uint8_t {
    text, keyword, type, function, number, string, comment, op, punct,
    constant, macro, property, special, heading, count
};
inline constexpr std::size_t kTokCount = static_cast<std::size_t>(Tok::count);

/// One highlighted run: bytes [start, end) of the line.
struct Span {
    std::uint32_t start = 0, end = 0;
    Tok kind = Tok::text;
};

/// What carries from one line to the next.
struct LineState {
    std::uint8_t block_comment = 0;   ///< inside /* ... */ (or the language's block)
    std::uint8_t string = 0;          ///< inside a multi-line string: the delimiter id
    std::uint8_t fence = 0;           ///< markdown: inside a ``` fence
    friend constexpr bool operator==(LineState, LineState) = default;
};

/// Identifier -> token kind, open-addressed. Built once per language by
/// detail::sorted(); the lexer asks it about every identifier on every line.
class WordTable {
  public:
    void add(std::string_view w, Tok k) {
        if (slots_.empty()) slots_.resize(64);
        if ((count_ + 1) * 2 > slots_.size()) grow();
        put(w, k);
    }
    [[nodiscard]] bool built() const noexcept { return !slots_.empty(); }
    /// Tok::text when `w` isn't a listed word.
    [[nodiscard]] Tok find(std::string_view w) const noexcept {
        if (w.size() > 24) return Tok::text;
        const std::size_t mask = slots_.size() - 1;
        for (std::size_t i = hash(w) & mask;; i = (i + 1) & mask) {
            const Slot& s = slots_[i];
            if (s.word.empty()) return Tok::text;
            if (s.word == w) return s.kind;
        }
    }

  private:
    struct Slot { std::string_view word; Tok kind = Tok::text; };
    static std::size_t hash(std::string_view w) noexcept {
        std::size_t h = 0x811c9dc5u ^ w.size();
        for (unsigned char c : w) h = (h ^ c) * 0x01000193u;
        return h ^ (h >> 15);
    }
    void put(std::string_view w, Tok k) {
        const std::size_t mask = slots_.size() - 1;
        for (std::size_t i = hash(w) & mask;; i = (i + 1) & mask) {
            if (slots_[i].word.empty()) { slots_[i] = {w, k}; ++count_; return; }
            if (slots_[i].word == w) return;   // first listing wins
        }
    }
    void grow() {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.size() * 2, Slot{});
        count_ = 0;
        for (const Slot& s : old) if (!s.word.empty()) put(s.word, s.kind);
    }
    std::vector<Slot> slots_;
    std::size_t       count_ = 0;
};

/// A language, as data. Adding one is filling in this struct.
struct Language {
    std::string_view              name{};
    std::vector<std::string_view> extensions{};
    std::vector<std::string_view> keywords{}, types{}, constants{}, specials{};
    std::string_view              line_comment{};
    std::string_view              block_open{}, block_close{};
    std::string_view              quotes = "\"'";
    bool hash_preproc     = false;   ///< C/C++ `#include`
    bool hash_attributes  = false;   ///< Rust `#[derive(..)]`
    bool at_decorators    = false;   ///< Python/TS `@decorator`
    bool triple_quotes    = false;   ///< Python """ docstrings """
    bool backtick_strings = false;   ///< JS template / Go raw strings, multi-line
    bool lifetimes        = false;   ///< Rust 'a
    bool bang_macros      = false;   ///< Rust println!
    bool capitalized_types = true;
    bool string_keys      = false;   ///< JSON: "key": is a property
    bool ini_like         = false;   ///< TOML: [section], key =
    bool markdown         = false;
    bool dollar_vars      = false;   ///< shell $VAR

    /// keywords/constants/types/specials hashed, in that priority. Filled by
    /// detail::sorted(); a hand-built Language without it still works.
    WordTable words{};
};

namespace detail {

[[nodiscard]] constexpr bool ident_start(unsigned char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}
[[nodiscard]] constexpr bool ident_char(unsigned char c) noexcept {
    return ident_start(c) || (c >= '0' && c <= '9');
}
[[nodiscard]] constexpr bool digit(unsigned char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] inline bool in(const std::vector<std::string_view>& sorted, std::string_view w) {
    return std::binary_search(sorted.begin(), sorted.end(), w);
}

inline Language sorted(Language l) {
    for (auto* v : {&l.keywords, &l.types, &l.constants, &l.specials}) std::sort(v->begin(), v->end());
    l.words = {};
    for (auto w : l.keywords)  l.words.add(w, Tok::keyword);
    for (auto w : l.constants) l.words.add(w, Tok::constant);
    for (auto w : l.types)     l.words.add(w, Tok::type);
    for (auto w : l.specials)  l.words.add(w, Tok::special);
    return l;
}

/// The listed kind of `w` (keyword, constant, type, special), or Tok::text.
[[nodiscard]] inline Tok classify(const Language& lang, std::string_view w) {
    if (lang.words.built()) return lang.words.find(w);
    if (in(lang.keywords, w)) return Tok::keyword;
    if (in(lang.constants, w)) return Tok::constant;
    if (in(lang.types, w)) return Tok::type;
    if (in(lang.specials, w)) return Tok::special;
    return Tok::text;
}

}  // namespace detail

/// Every built-in language. Stable addresses: callers keep `const Language*`.
[[nodiscard]] inline const std::vector<Language>& languages() {
    static const std::vector<Language> all = [] {
        using detail::sorted;
        std::vector<Language> v;
        v.push_back(sorted(Language{
            .name = "C++",
            .extensions = {"cpp", "cc", "cxx", "hpp", "hh", "hxx", "h", "c", "ipp", "inl", "mm", "m"},
            .keywords = {"alignas", "alignof", "auto", "break", "case", "catch", "class", "co_await",
                         "co_return", "co_yield", "concept", "const", "consteval", "constexpr",
                         "constinit", "const_cast", "continue", "decltype", "default", "delete", "do",
                         "dynamic_cast", "else", "enum", "explicit", "export", "extern", "final",
                         "for", "friend", "goto", "if", "import", "inline", "module", "mutable",
                         "namespace", "new", "noexcept", "operator", "override", "private",
                         "protected", "public", "reinterpret_cast", "requires", "return", "sizeof",
                         "static", "static_assert", "static_cast", "struct", "switch", "template",
                         "thread_local", "throw", "try", "typedef", "typeid", "typename", "union",
                         "using", "virtual", "volatile", "while"},
            .types = {"bool", "char", "char8_t", "char16_t", "char32_t", "double", "float", "int",
                      "long", "short", "signed", "unsigned", "void", "wchar_t", "size_t", "ssize_t",
                      "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
                      "uint64_t", "ptrdiff_t", "nullptr_t", "string", "string_view", "vector",
                      "array", "span", "optional", "variant", "map", "set", "unordered_map",
                      "unique_ptr", "shared_ptr", "function", "pair", "tuple"},
            .constants = {"true", "false", "nullptr", "NULL"},
            .specials = {"this", "std"},
            .line_comment = "//", .block_open = "/*", .block_close = "*/",
            .hash_preproc = true}));
        v.push_back(sorted(Language{
            .name = "Rust",
            .extensions = {"rs"},
            .keywords = {"as", "async", "await", "break", "const", "continue", "crate", "dyn",
                         "else", "enum", "extern", "fn", "for", "if", "impl", "in", "let", "loop",
                         "match", "mod", "move", "mut", "pub", "ref", "return", "static", "struct",
                         "super", "trait", "type", "unsafe", "use", "where", "while", "yield"},
            .types = {"bool", "char", "f32", "f64", "i8", "i16", "i32", "i64", "i128", "isize",
                      "str", "u8", "u16", "u32", "u64", "u128", "usize", "String", "Vec", "Option",
                      "Result", "Box", "Rc", "Arc", "HashMap", "HashSet"},
            .constants = {"true", "false", "None", "Some", "Ok", "Err"},
            .specials = {"self", "Self"},
            .line_comment = "//", .block_open = "/*", .block_close = "*/",
            .quotes = "\"", .hash_attributes = true, .lifetimes = true, .bang_macros = true}));
        v.push_back(sorted(Language{
            .name = "Python",
            .extensions = {"py", "pyi", "pyw"},
            .keywords = {"and", "as", "assert", "async", "await", "break", "class", "continue", "def",
                         "del", "elif", "else", "except", "finally", "for", "from", "global", "if",
                         "import", "in", "is", "lambda", "match", "case", "nonlocal", "not", "or",
                         "pass", "raise", "return", "try", "while", "with", "yield"},
            .types = {"int", "float", "str", "bytes", "bool", "list", "dict", "set", "tuple",
                      "object", "type", "Exception"},
            .constants = {"True", "False", "None"},
            .specials = {"self", "cls", "print", "len", "range", "super"},
            .line_comment = "#", .at_decorators = true, .triple_quotes = true}));
        v.push_back(sorted(Language{
            .name = "TypeScript",
            .extensions = {"ts", "tsx", "js", "jsx", "mjs", "cjs", "mts"},
            .keywords = {"abstract", "as", "async", "await", "break", "case", "catch", "class",
                         "const", "continue", "debugger", "declare", "default", "delete", "do",
                         "else", "enum", "export", "extends", "finally", "for", "from", "function",
                         "get", "if", "implements", "import", "in", "instanceof", "interface", "is",
                         "keyof", "let", "namespace", "new", "of", "private", "protected",
                         "public", "readonly", "return", "set", "static", "switch", "throw", "try",
                         "type", "typeof", "var", "void", "while", "with", "yield"},
            .types = {"any", "boolean", "never", "number", "object", "string", "symbol", "unknown",
                      "bigint", "Array", "Promise", "Record", "Map", "Set"},
            .constants = {"true", "false", "null", "undefined", "NaN", "Infinity"},
            .specials = {"this", "super", "console", "window", "document"},
            .line_comment = "//", .block_open = "/*", .block_close = "*/",
            .quotes = "\"'`", .at_decorators = true, .backtick_strings = true}));
        v.push_back(sorted(Language{
            .name = "Go",
            .extensions = {"go"},
            .keywords = {"break", "case", "chan", "const", "continue", "default", "defer", "else",
                         "fallthrough", "for", "func", "go", "goto", "if", "import", "interface",
                         "map", "package", "range", "return", "select", "struct", "switch", "type",
                         "var"},
            .types = {"bool", "byte", "complex64", "complex128", "error", "float32", "float64",
                      "int", "int8", "int16", "int32", "int64", "rune", "string", "uint", "uint8",
                      "uint16", "uint32", "uint64", "uintptr", "any"},
            .constants = {"true", "false", "nil", "iota"},
            .specials = {"make", "new", "len", "cap", "append", "panic"},
            .line_comment = "//", .block_open = "/*", .block_close = "*/",
            .quotes = "\"'`", .backtick_strings = true, .capitalized_types = false}));
        v.push_back(sorted(Language{
            .name = "JSON", .extensions = {"json", "jsonc", "json5"},
            .constants = {"true", "false", "null"},
            .line_comment = "//", .quotes = "\"", .capitalized_types = false, .string_keys = true}));
        v.push_back(sorted(Language{
            .name = "TOML", .extensions = {"toml"},
            .constants = {"true", "false"},
            .line_comment = "#", .quotes = "\"'", .capitalized_types = false, .ini_like = true}));
        v.push_back(sorted(Language{
            .name = "Shell", .extensions = {"sh", "bash", "zsh", "fish"},
            .keywords = {"if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done",
                         "case", "esac", "in", "function", "return", "local", "export", "readonly"},
            .specials = {"echo", "cd", "exit", "set", "source", "printf", "test"},
            .line_comment = "#", .capitalized_types = false, .dollar_vars = true}));
        v.push_back(sorted(Language{
            .name = "Markdown", .extensions = {"md", "markdown"},
            .quotes = "", .capitalized_types = false, .markdown = true}));
        // what you edit on a server: config of every dialect
        v.push_back(sorted(Language{
            .name = "YAML", .extensions = {"yml", "yaml"},
            .constants = {"true", "false", "null", "yes", "no", "on", "off", "True", "False", "~"},
            .line_comment = "#", .quotes = "\"'", .capitalized_types = false, .ini_like = true}));
        v.push_back(sorted(Language{
            .name = "Config",
            .extensions = {"conf", "cfg", "ini", "cnf", "env", "properties", "service", "socket", "timer",
                           "mount", "target", "desktop", "gitconfig", "editorconfig", ".env", "config",
                           "sshd_config", "ssh_config", "hosts", "fstab", "crontab"},
            .constants = {"true", "false", "yes", "no", "on", "off"},
            .line_comment = "#", .quotes = "\"'", .capitalized_types = false, .ini_like = true}));
        v.push_back(sorted(Language{
            .name = "Dockerfile", .extensions = {"Dockerfile", "Containerfile", "dockerfile"},
            .keywords = {"FROM", "RUN", "CMD", "LABEL", "EXPOSE", "ENV", "ADD", "COPY", "ENTRYPOINT",
                         "VOLUME", "USER", "WORKDIR", "ARG", "ONBUILD", "STOPSIGNAL", "HEALTHCHECK",
                         "SHELL", "AS"},
            .line_comment = "#", .quotes = "\"'", .capitalized_types = false, .dollar_vars = true}));
        v.push_back(sorted(Language{
            .name = "Makefile", .extensions = {"Makefile", "makefile", "GNUmakefile", "mk", "mak"},
            .keywords = {"ifeq", "ifneq", "ifdef", "ifndef", "else", "endif", "include", "define",
                         "endef", "export", "override", "PHONY"},
            .line_comment = "#", .quotes = "\"'", .capitalized_types = false, .dollar_vars = true}));
        v.push_back(sorted(Language{
            .name = "CMake", .extensions = {"cmake", "CMakeLists.txt"},
            .keywords = {"if", "elseif", "else", "endif", "foreach", "endforeach", "while", "endwhile",
                         "function", "endfunction", "macro", "endmacro", "return"},
            .constants = {"ON", "OFF", "TRUE", "FALSE", "YES", "NO", "PUBLIC", "PRIVATE", "INTERFACE"},
            .line_comment = "#", .quotes = "\"", .capitalized_types = false, .dollar_vars = true}));
        v.push_back(sorted(Language{.name = "Plain Text", .extensions = {"txt"}, .quotes = "",
                                    .capitalized_types = false}));
        return v;
    }();
    return all;
}

/// The language for a file name, or Plain Text. The whole name wins over the
/// extension (`CMakeLists.txt`, `Dockerfile`, `.env`), then the extension.
[[nodiscard]] inline const Language& language_for(std::string_view path) {
    const auto& all = languages();
    const auto slash = path.find_last_of("/\\");
    const std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
    for (const auto& l : all)
        for (auto e : l.extensions)
            if (e == base) return l;
    const auto dot = base.rfind('.');
    const std::string_view ext = dot == std::string_view::npos ? std::string_view{} : base.substr(dot + 1);
    if (!ext.empty())
        for (const auto& l : all)
            for (auto e : l.extensions)
                if (e == ext) return l;
    // Dockerfile.dev, Makefile.inc
    const std::string_view stem = dot == std::string_view::npos ? base : base.substr(0, dot);
    for (const auto& l : all)
        for (auto e : l.extensions)
            if (e.size() > 4 && e == stem) return l;
    return all.back();
}
[[nodiscard]] inline const Language& language_named(std::string_view name) {
    for (const auto& l : languages()) if (l.name == name) return l;
    return languages().back();
}

/// Highlight one line. Appends spans (whitespace is not emitted) and returns
/// the state the next line starts in.
inline LineState highlight_line(const Language& lang, std::string_view s, LineState st,
                                std::vector<Span>& out) {
    using namespace detail;
    const std::size_t n = s.size();
    std::size_t i = 0;
    const auto emit = [&](std::size_t a, std::size_t b, Tok k) {
        if (b > a) out.push_back(Span{static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b), k});
    };
    const auto starts = [&](std::size_t at, std::string_view w) {
        return !w.empty() && at + w.size() <= n && s.substr(at, w.size()) == w;
    };
    const auto next_non_space = [&](std::size_t at) {
        while (at < n && (s[at] == ' ' || s[at] == '\t')) ++at;
        return at < n ? s[at] : '\0';
    };
    const auto prev_non_space = [&](std::size_t at) -> char {
        while (at > 0 && (s[at - 1] == ' ' || s[at - 1] == '\t')) --at;
        return at > 0 ? s[at - 1] : '\0';
    };

    // ── markdown is line-structured, not token-structured ───────────────
    if (lang.markdown) {
        std::size_t lead = 0;
        while (lead < n && s[lead] == ' ') ++lead;
        if (starts(lead, "```")) { emit(lead, n, Tok::string); st.fence = st.fence ? 0 : 1; return st; }
        if (st.fence) { emit(0, n, Tok::string); return st; }
        if (lead < n && s[lead] == '#') { emit(lead, n, Tok::heading); return st; }
        if (lead < n && s[lead] == '>') { emit(lead, n, Tok::comment); return st; }
        if (lead + 1 < n && (s[lead] == '-' || s[lead] == '*' || s[lead] == '+') && s[lead + 1] == ' ') {
            emit(lead, lead + 1, Tok::punct);
            i = lead + 1;
        }
        while (i < n) {
            if (s[i] == '`') {
                const auto e = s.find('`', i + 1);
                const std::size_t end = e == std::string_view::npos ? n : e + 1;
                emit(i, end, Tok::string); i = end; continue;
            }
            if (s[i] == '[') {
                const auto c = s.find("](", i);
                const auto e = c == std::string_view::npos ? c : s.find(')', c);
                if (e != std::string_view::npos) {
                    emit(i, c + 1, Tok::function); emit(c + 1, e + 1, Tok::comment); i = e + 1; continue;
                }
            }
            if (starts(i, "**")) {
                const auto e = s.find("**", i + 2);
                if (e != std::string_view::npos) { emit(i, e + 2, Tok::keyword); i = e + 2; continue; }
            }
            std::size_t j = i;
            while (j < n && s[j] != '`' && s[j] != '[' && !(s[j] == '*' && j + 1 < n && s[j + 1] == '*')) ++j;
            if (j == i) ++j;
            emit(i, j, Tok::text);
            i = j;
        }
        return st;
    }

    // ── continuing constructs from the previous line ────────────────────
    if (st.block_comment) {
        const auto e = s.find(lang.block_close);
        if (e == std::string_view::npos) { emit(0, n, Tok::comment); return st; }
        emit(0, e + lang.block_close.size(), Tok::comment);
        i = e + lang.block_close.size();
        st.block_comment = 0;
    }
    if (st.string) {
        const std::string_view close = st.string == 1 ? "\"\"\"" : st.string == 2 ? "'''" : "`";
        std::size_t j = 0;
        bool closed = false;
        while (j < n) {
            if (s[j] == '\\') { j += 2; continue; }
            if (starts(j, close)) { j += close.size(); closed = true; break; }
            ++j;
        }
        emit(0, std::min(j, n), Tok::string);
        if (!closed) return st;
        i = std::min(j, n);
        st.string = 0;
    }

    // Leading `#` lines: C preprocessor.
    if (lang.hash_preproc) {
        std::size_t lead = 0;
        while (lead < n && (s[lead] == ' ' || s[lead] == '\t')) ++lead;
        if (lead < n && s[lead] == '#' && i == 0) {
            std::size_t j = lead + 1;
            while (j < n && s[j] == ' ') ++j;
            while (j < n && ident_char(static_cast<unsigned char>(s[j]))) ++j;
            emit(lead, j, Tok::macro);
            i = j;
            while (i < n && s[i] == ' ') ++i;
            if (i < n && s[i] == '<') {
                const auto e = s.find('>', i);
                if (e != std::string_view::npos) { emit(i, e + 1, Tok::string); i = e + 1; }
            }
        }
    }

    // TOML/INI [section], key = value; YAML key: value, - key: value
    if (lang.ini_like) {
        std::size_t lead = 0;
        while (lead < n && s[lead] == ' ') ++lead;
        if (lead + 1 < n && s[lead] == '-' && s[lead + 1] == ' ') { lead += 2; while (lead < n && s[lead] == ' ') ++lead; i = lead; }
        if (lead < n && s[lead] == '[') {
            const auto e = s.find(']', lead);
            if (e != std::string_view::npos) { emit(lead, e + 1, Tok::type); i = e + 1; }
        } else if (lead < n && ident_start(static_cast<unsigned char>(s[lead]))) {
            std::size_t j = lead;
            while (j < n && (ident_char(static_cast<unsigned char>(s[j])) || s[j] == '-' || s[j] == '.')) ++j;
            std::size_t k = j;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
            const bool colon = k < n && s[k] == ':' && (k + 1 == n || s[k + 1] == ' ' || s[k + 1] == '\t');   // not a URL's "http://"
            if ((k < n && s[k] == '=') || colon) { emit(lead, j, Tok::property); i = j; }
        }
    }

    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ' ' || c == '\t') { ++i; continue; }

        // comments
        if (starts(i, lang.line_comment)) { emit(i, n, Tok::comment); return st; }
        if (starts(i, lang.block_open)) {
            const auto e = s.find(lang.block_close, i + lang.block_open.size());
            if (e == std::string_view::npos) { emit(i, n, Tok::comment); st.block_comment = 1; return st; }
            emit(i, e + lang.block_close.size(), Tok::comment);
            i = e + lang.block_close.size();
            continue;
        }

        // attributes, decorators
        if (lang.hash_attributes && c == '#' && i + 1 < n && (s[i + 1] == '[' || s[i + 1] == '!')) {
            int depth = 0;
            std::size_t j = i + 1;
            for (; j < n; ++j) {
                if (s[j] == '[') ++depth;
                else if (s[j] == ']' && --depth == 0) { ++j; break; }
            }
            emit(i, j, Tok::macro); i = j; continue;
        }
        if (lang.at_decorators && c == '@' && i + 1 < n && ident_start(static_cast<unsigned char>(s[i + 1]))) {
            std::size_t j = i + 1;
            while (j < n && (ident_char(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
            emit(i, j, Tok::macro); i = j; continue;
        }
        if (lang.dollar_vars && c == '$') {
            std::size_t j = i + 1;
            if (j < n && s[j] == '{') { const auto e = s.find('}', j); j = e == std::string_view::npos ? n : e + 1; }
            else while (j < n && (ident_char(static_cast<unsigned char>(s[j])) || s[j] == '?' || s[j] == '@')) ++j;
            emit(i, j, Tok::special); i = j; continue;
        }

        // lifetimes: 'a (but 'a' is a char)
        if (lang.lifetimes && c == '\'' && i + 2 < n && ident_start(static_cast<unsigned char>(s[i + 1])) &&
            s[i + 2] != '\'') {
            std::size_t j = i + 1;
            while (j < n && ident_char(static_cast<unsigned char>(s[j]))) ++j;
            emit(i, j, Tok::special); i = j; continue;
        }

        // strings
        if (lang.triple_quotes && (starts(i, "\"\"\"") || starts(i, "'''"))) {
            const std::string_view q = s.substr(i, 3);
            const auto e = s.find(q, i + 3);
            if (e == std::string_view::npos) { emit(i, n, Tok::string); st.string = q[0] == '"' ? 1 : 2; return st; }
            emit(i, e + 3, Tok::string); i = e + 3; continue;
        }
        if (lang.quotes.find(static_cast<char>(c)) != std::string_view::npos) {
            std::size_t j = i + 1;
            bool closed = false;
            while (j < n) {
                if (s[j] == '\\') { j += 2; continue; }
                if (static_cast<unsigned char>(s[j]) == c) { ++j; closed = true; break; }
                ++j;
            }
            j = std::min(j, n);
            if (!closed && c == '`' && lang.backtick_strings) { emit(i, n, Tok::string); st.string = 3; return st; }
            emit(i, j, (lang.string_keys && next_non_space(j) == ':') ? Tok::property : Tok::string);
            i = j;
            continue;
        }

        // numbers
        if (digit(c) || (c == '.' && i + 1 < n && digit(static_cast<unsigned char>(s[i + 1])))) {
            std::size_t j = i + 1;
            while (j < n && (ident_char(static_cast<unsigned char>(s[j])) || s[j] == '.' || s[j] == '\'' ||
                             ((s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E'))))
                ++j;
            emit(i, j, Tok::number); i = j; continue;
        }

        // identifiers
        if (ident_start(c)) {
            std::size_t j = i + 1;
            while (j < n && ident_char(static_cast<unsigned char>(s[j]))) ++j;
            const std::string_view w = s.substr(i, j - i);
            Tok k = classify(lang, w);
            if (k != Tok::text) {}
            else if (lang.bang_macros && j < n && s[j] == '!') { k = Tok::macro; ++j; }
            else if (next_non_space(j) == '(') k = Tok::function;
            else if (prev_non_space(i) == '.' || (i >= 2 && s.substr(i - 2, 2) == "->")) k = Tok::property;
            else if (lang.capitalized_types && w[0] >= 'A' && w[0] <= 'Z') {
                const bool all_caps = w.size() > 1 && std::all_of(w.begin(), w.end(), [](char ch) {
                    return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
                });
                k = all_caps ? Tok::constant : Tok::type;
            }
            emit(i, j, k);
            i = j;
            continue;
        }

        // operators and punctuation, one run each
        constexpr std::string_view ops = "+-*/%=<>!&|^~?:";
        constexpr std::string_view puncts = "(){}[];,.";
        if (ops.find(static_cast<char>(c)) != std::string_view::npos) {
            std::size_t j = i + 1;
            while (j < n && ops.find(s[j]) != std::string_view::npos && !starts(j, lang.line_comment) &&
                   !starts(j, lang.block_open))
                ++j;
            emit(i, j, Tok::op); i = j; continue;
        }
        if (puncts.find(static_cast<char>(c)) != std::string_view::npos) { emit(i, i + 1, Tok::punct); ++i; continue; }
        // anything else (UTF-8 lead bytes are ident_start; this is stray ASCII)
        emit(i, i + 1, Tok::text);
        ++i;
    }
    return st;
}

/// Per-line start states, kept warm across edits. Lives inside the editor
/// model; view() reads through it, so it is `mutable` derived data like
/// ScrollState's measurements.
class SyntaxCache {
  public:
    /// Lines from `line` on must be relexed.
    void invalidate_from(int line) noexcept { valid_ = std::min(valid_, std::max(line, 0)); }
    void reset() noexcept { valid_ = 0; states_.clear(); }

    /// The state line `line` starts in, computing (and caching) every state
    /// above it that is stale.
    LineState state_at(const Language& lang, std::span<const std::string> lines, int line) const {
        if (lang_ != &lang) { lang_ = &lang; valid_ = 0; }
        line = std::clamp(line, 0, static_cast<int>(lines.size()));
        if (states_.size() < lines.size() + 1) states_.resize(lines.size() + 1);
        if (valid_ == 0) states_[0] = LineState{};
        std::vector<Span> scratch;
        while (valid_ < line) {
            scratch.clear();
            states_[static_cast<std::size_t>(valid_) + 1] =
                highlight_line(lang, lines[static_cast<std::size_t>(valid_)], states_[static_cast<std::size_t>(valid_)], scratch);
            ++valid_;
        }
        return states_[static_cast<std::size_t>(line)];
    }

    /// Spans of one line.
    std::vector<Span> spans(const Language& lang, std::span<const std::string> lines, int line) const {
        std::vector<Span> out;
        if (line < 0 || line >= static_cast<int>(lines.size())) return out;
        highlight_line(lang, lines[static_cast<std::size_t>(line)], state_at(lang, lines, line), out);
        return out;
    }

  private:
    mutable std::vector<LineState> states_;
    mutable int                    valid_ = 0;
    mutable const Language*        lang_ = nullptr;
};

// ── outline ─────────────────────────────────────────────────────────────

struct Symbol {
    std::string name;
    std::string kind;    ///< "fn", "struct", "class", ...
    int         line = 0;
    int         depth = 0;
};

/// Top-level-ish symbols, by lexing each line and looking for an introducing
/// keyword (`fn`, `class`, `def`, ...) or, in C++, a function defined at the
/// start of a line. A heuristic; good enough for breadcrumbs and Go to
/// Symbol, and costs one lex per line.
[[nodiscard]] inline std::vector<Symbol> outline(const Language& lang, std::span<const std::string> lines,
                                                 int tab_width = 4) {
    static constexpr std::array<std::string_view, 19> introducers{
        "fn", "struct", "enum", "trait", "impl", "mod", "class", "def", "function", "interface",
        "type", "namespace", "union", "func", "concept", "module", "package", "const", "macro"};
    std::vector<Symbol> out;
    LineState st{};
    std::vector<Span> spans;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const std::string& s = lines[li];
        spans.clear();
        const LineState before = st;
        st = highlight_line(lang, s, st, spans);
        if (before.block_comment || before.string || spans.empty()) continue;
        int indent = 0;
        for (char ch : s) { if (ch == ' ') ++indent; else if (ch == '\t') indent += tab_width; else break; }
        const int depth = indent / std::max(tab_width, 1);
        if (depth > 2) continue;
        for (std::size_t k = 0; k < spans.size(); ++k) {
            const auto w = std::string_view{s}.substr(spans[k].start, spans[k].end - spans[k].start);
            if (spans[k].kind == Tok::keyword &&
                std::find(introducers.begin(), introducers.end(), w) != introducers.end()) {
                if (w == "const" && lang.name != "TypeScript") continue;
                for (std::size_t q = k + 1; q < spans.size(); ++q) {
                    const auto kind = spans[q].kind;
                    if (kind == Tok::text || kind == Tok::type || kind == Tok::function || kind == Tok::constant) {
                        out.push_back(Symbol{std::string{s.substr(spans[q].start, spans[q].end - spans[q].start)},
                                             std::string{w}, static_cast<int>(li), depth});
                        break;
                    }
                    if (kind == Tok::punct) break;
                }
                break;
            }
            // C++/C: `ret name(args) {` at the start of a line or one level in.
            if (lang.name == "C++" && spans[k].kind == Tok::function && depth <= 1) {
                const auto tail = s.find_last_not_of(" \t");
                const char last = tail == std::string::npos ? '\0' : s[tail];
                const bool control = spans[0].kind == Tok::keyword &&
                    (std::string_view{s}.substr(spans[0].start, spans[0].end - spans[0].start) == "return" ||
                     std::string_view{s}.substr(spans[0].start, spans[0].end - spans[0].start) == "if" ||
                     std::string_view{s}.substr(spans[0].start, spans[0].end - spans[0].start) == "while" ||
                     std::string_view{s}.substr(spans[0].start, spans[0].end - spans[0].start) == "for");
                if (k > 0 && last != ';' && last != ',' && !control) {
                    out.push_back(Symbol{std::string{w}, "fn", static_cast<int>(li), depth});
                }
                break;
            }
        }
    }
    return out;
}

}  // namespace mayag::editor
