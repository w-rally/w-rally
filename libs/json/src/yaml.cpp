#include "wrally/formats/json.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

// The YAML subset W-Rally's manifests use (json.h): block mappings and sequences by indentation, flow [..] and {..}
// (so a JSON text is YAML too), plain, '' and "" scalars, | and > blocks, comments. Every scalar is a String, but a
// plain null, ~ or nothing, which is a Null.

namespace wrally::formats {

namespace {

struct Line {
    int indent = 0;          // spaces before the text
    std::string text;        // after them, trailing spaces cut ("" a blank line)
    size_t begin = 0;        // the raw line's offset in the source
    size_t textAt = 0;       // the text's
    size_t no = 0;           // 1-based
};

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool IsSkipped(const Line& l) { return l.text.empty() || l.text[0] == '#'; }
bool IsDocMarker(const Line& l) { return l.indent == 0 && (l.text == "..." || l.text.rfind("---", 0) == 0); }
bool IsSeqItem(const std::string& t) { return t == "-" || (t.size() > 1 && t[0] == '-' && (t[1] == ' ' || t[1] == '\t')); }

std::string TrimRight(std::string s)
{
    while (!s.empty() && IsSpace(s.back())) s.pop_back();
    return s;
}

// A plain scalar's text: up to a comment (" #").
std::string PlainEnd(const std::string& t)
{
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i] == '#' && (i == 0 || t[i - 1] == ' ' || t[i - 1] == '\t')) return TrimRight(t.substr(0, i));
    return TrimRight(t);
}

void Utf8(uint32_t c, std::string& out)
{
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) out += static_cast<char>(0xc0 | c >> 6), out += static_cast<char>(0x80 | (c & 0x3f));
    else if (c < 0x10000)
        out += static_cast<char>(0xe0 | c >> 12), out += static_cast<char>(0x80 | (c >> 6 & 0x3f)), out += static_cast<char>(0x80 | (c & 0x3f));
    else
        out += static_cast<char>(0xf0 | c >> 18), out += static_cast<char>(0x80 | (c >> 12 & 0x3f)),
            out += static_cast<char>(0x80 | (c >> 6 & 0x3f)), out += static_cast<char>(0x80 | (c & 0x3f));
}

Json Scalar(std::string text, bool plain)
{
    Json v;
    if (plain && (text.empty() || text == "~" || text == "null" || text == "Null" || text == "NULL")) return v;
    v.type = Json::String;
    v.text = std::move(text);
    return v;
}

class Reader {
public:
    explicit Reader(const std::string& src) : m_src(src)
    {
        size_t at = src.size() >= 3 && !src.compare(0, 3, "\xEF\xBB\xBF") ? 3 : 0;   // (a UTF-8 BOM)
        for (size_t no = 1; at <= src.size(); ++no) {
            size_t end = src.find('\n', at);
            if (end == std::string::npos) end = src.size();
            Line l;
            l.begin = at, l.no = no;
            size_t i = at;
            while (i < end && src[i] == ' ') ++i;
            l.indent = static_cast<int>(i - at);
            l.textAt = i;
            l.text = TrimRight(src.substr(i, end - i));
            if (!l.text.empty() && l.text[0] == '\t') {
                const std::string rest = TrimRight(l.text);
                size_t k = 0;
                while (k < rest.size() && (rest[k] == ' ' || rest[k] == '\t')) ++k;
                if (k < rest.size()) Fail("a tab in the indentation (spaces only)", no);
                l.text.clear();
            }
            m_lines.push_back(std::move(l));
            at = end + 1;
        }
    }

    Json Document()
    {
        Line* l = Cur();
        if (l && l->indent == 0 && l->text[0] == '%') Fail("YAML directives are not supported", l->no);
        if (l && l->indent == 0 && l->text.rfind("---", 0) == 0) {
            if (PlainEnd(l->text) != "---") Fail("a value on the '---' line (put it on the next)", l->no);
            ++m_at;
        }
        Json v;
        if ((l = Cur()) && !(l->indent == 0 && l->text == "...")) v = Block(-1, 0);
        if ((l = Cur()) && l->indent == 0 && l->text == "...") ++m_at, l = Cur();
        if (l) Fail(l->text == "---" ? "several documents (one only)" : "bad indentation", l->no);
        return v;
    }

private:
    const std::string& m_src;
    std::vector<Line> m_lines;
    size_t m_at = 0;     // the current line
    size_t m_p = 0;      // the flow scanner's offset in m_src

    [[noreturn]] void Fail(const std::string& what, size_t no) const
    {
        throw std::runtime_error(what + " (line " + std::to_string(no) + ")");
    }
    [[noreturn]] void FailAt(const std::string& what, size_t offset) const
    {
        size_t no = 1;
        for (size_t i = 0; i < offset && i < m_src.size(); ++i) no += m_src[i] == '\n';
        Fail(what, no);
    }

    // The current line past blank and comment lines (nullptr: the end).
    Line* Cur()
    {
        while (m_at < m_lines.size() && IsSkipped(m_lines[m_at])) ++m_at;
        return m_at < m_lines.size() ? &m_lines[m_at] : nullptr;
    }
    // The line's text from `from` on, past spaces.
    static void Cut(Line& l, size_t from)
    {
        while (from < l.text.size() && (l.text[from] == ' ' || l.text[from] == '\t')) ++from;
        l.text = l.text.substr(from);
        l.textAt += from;
    }

    // A "key:"'s colon in a line's text, else npos.
    static size_t KeyColon(const std::string& t)
    {
        if (t.empty() || t[0] == '[' || t[0] == '{' || IsSeqItem(t)) return std::string::npos;
        size_t i = 0;
        if (t[0] == '"' || t[0] == '\'') {
            const char q = t[0];
            for (i = 1; i < t.size(); ++i) {
                if (q == '"' && t[i] == '\\') ++i;
                else if (t[i] == q && q == '\'' && i + 1 < t.size() && t[i + 1] == '\'') ++i;
                else if (t[i] == q) break;
            }
            if (i >= t.size()) return std::string::npos;
            ++i;
            while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
            return i < t.size() && t[i] == ':' && (i + 1 == t.size() || t[i + 1] == ' ' || t[i + 1] == '\t') ? i : std::string::npos;
        }
        for (; i < t.size(); ++i) {
            if (t[i] == '#' && i > 0 && (t[i - 1] == ' ' || t[i - 1] == '\t')) return std::string::npos;
            if (t[i] == ':' && (i + 1 == t.size() || t[i + 1] == ' ' || t[i + 1] == '\t')) return i;
        }
        return std::string::npos;
    }

    // The node starting at the current line, indented more than `parent`.
    Json Block(int parent, int depth)
    {
        if (depth > 64) Fail("nested too deep", m_lines[m_at].no);
        Line& l = *Cur();
        if (IsSeqItem(l.text)) return Sequence(l.indent, depth);
        if (KeyColon(l.text) != std::string::npos) return Mapping(l.indent, depth);
        return Inline(parent);
    }

    // What an empty "key:" or "-" holds: the more indented lines after it, else nothing.
    Json Child(int parent, int depth)
    {
        const Line* l = Cur();
        return l && l->indent > parent ? Block(parent, depth + 1) : Json{};
    }

    Json Sequence(int n, int depth)
    {
        Json v;
        v.type = Json::Array;
        for (Line* l = Cur(); l && l->indent == n && IsSeqItem(l->text) && !IsDocMarker(*l); l = Cur()) {
            const size_t indentBefore = l->textAt;
            Cut(*l, 1);
            if (IsSkipped(*l)) {
                ++m_at;
                v.items.push_back(Child(n, depth));
            } else {
                l->indent = n + static_cast<int>(l->textAt - indentBefore);   // the rest is a node at its own column
                v.items.push_back(Block(n, depth + 1));
            }
        }
        if (const Line* l = Cur(); l && l->indent > n) Fail("bad indentation", l->no);
        return v;
    }

    Json Mapping(int n, int depth)
    {
        Json v;
        v.type = Json::Object;
        for (Line* l = Cur(); l && l->indent == n && !IsDocMarker(*l); l = Cur()) {
            if (IsSeqItem(l->text)) Fail("a '- ' item where a key was expected", l->no);
            const size_t colon = KeyColon(l->text);
            if (colon == std::string::npos) Fail("'key: value' expected", l->no);
            std::string key = Key(*l, colon);
            if (v.Get(key)) Fail("the key \"" + key + "\" twice", l->no);
            Cut(*l, colon + 1);
            Json value;
            if (IsSkipped(*l)) {
                ++m_at;
                const Line* next = Cur();
                if (next && next->indent == n && IsSeqItem(next->text)) value = Sequence(n, depth + 1);   // (a list may sit at its key's indent)
                else value = Child(n, depth);
            } else {
                value = Inline(n);
            }
            v.members.emplace_back(std::move(key), std::move(value));
        }
        if (const Line* l = Cur(); l && l->indent > n) Fail("bad indentation", l->no);
        return v;
    }

    std::string Key(const Line& l, size_t colon)
    {
        const char c = l.text[0];
        if (c == '"' || c == '\'') {
            m_p = l.textAt;
            return Quoted();
        }
        if (c == '?') Fail("complex keys ('? ') are not supported", l.no);
        if (c == '&' || c == '*' || c == '!') Fail("anchors, aliases and tags are not supported", l.no);
        std::string key = TrimRight(l.text.substr(0, colon));
        if (key.empty()) Fail("a key expected", l.no);
        return key;
    }

    // The current line's text as one value (a scalar, a flow collection, a | or > block); `parent` the indent its
    // continuation lines must be deeper than.
    Json Inline(int parent)
    {
        Line& l = m_lines[m_at];
        const char c = l.text[0];
        if (c == '&' || c == '*' || c == '!') Fail("anchors, aliases and tags are not supported", l.no);
        if (c == '|' || c == '>') return BlockScalar(parent);
        if (c == '[' || c == '{' || c == '"' || c == '\'') {
            m_p = l.textAt;
            Json v = c == '[' || c == '{' ? Flow(0) : Scalar(Quoted(), false);
            // the rest of its last line: nothing, or a comment
            while (m_p < m_src.size() && (m_src[m_p] == ' ' || m_src[m_p] == '\t' || m_src[m_p] == '\r')) ++m_p;
            if (m_p < m_src.size() && m_src[m_p] != '\n' && m_src[m_p] != '#') FailAt("text after the value", m_p);
            while (m_at < m_lines.size() && m_lines[m_at].begin <= m_p) ++m_at;
            return v;
        }
        if (c == '%' || c == '@' || c == '`') Fail(std::string("a value starting with '") + c + "' - quote it", l.no);
        if (IsSeqItem(l.text)) Fail("a list starts on its own line", l.no);
        std::string s = PlainEnd(l.text);
        ++m_at;
        // a plain scalar may go on, folded, in deeper lines
        while (m_at < m_lines.size()) {
            const Line& next = m_lines[m_at];
            if (IsSkipped(next) || IsDocMarker(next) || next.indent <= parent || IsSeqItem(next.text) ||
                KeyColon(next.text) != std::string::npos)
                break;
            s += ' ' + PlainEnd(next.text);
            ++m_at;
        }
        if (s.find(": ") != std::string::npos || s.back() == ':')
            Fail("a ':' in a value - quote it (\"...\")", l.no);
        return Scalar(std::move(s), true);
    }

    // | (lines kept) or > (folded), with - (no last line break) or + (every one) and an indent digit.
    Json BlockScalar(int parent)
    {
        const Line& head = m_lines[m_at];
        const bool literal = head.text[0] == '|';
        char chomp = 0;
        int indent = 0;
        size_t i = 1;
        for (; i < head.text.size() && i < 3; ++i) {
            const char c = head.text[i];
            if ((c == '-' || c == '+') && !chomp) chomp = c;
            else if (c >= '1' && c <= '9' && !indent) indent = c - '0';
            else break;
        }
        if (const std::string rest = PlainEnd(head.text.substr(i)); !rest.empty() && rest.find_first_not_of(" \t") != std::string::npos)
            Fail("text after a | or > block's header", head.no);
        ++m_at;
        int ci = indent ? std::max(parent, 0) + indent : -1;   // the content's indent
        if (ci < 0)
            for (size_t k = m_at; k < m_lines.size(); ++k)
                if (!m_lines[k].text.empty()) {
                    ci = m_lines[k].indent;
                    break;
                }
        std::vector<std::string> lines;
        if (ci > parent)
            for (; m_at < m_lines.size(); ++m_at) {
                const Line& l = m_lines[m_at];
                if (l.text.empty()) lines.emplace_back();
                else if (l.indent >= ci) lines.push_back(std::string(static_cast<size_t>(l.indent - ci), ' ') + l.text);
                else break;
            }
        size_t trailing = 0;
        while (!lines.empty() && lines.back().empty()) lines.pop_back(), ++trailing;
        std::string out;
        if (literal) {
            for (size_t k = 0; k < lines.size(); ++k) out += (k ? "\n" : "") + lines[k];
        } else {
            bool first = true, prevMore = false;
            size_t breaks = 0;
            for (const std::string& s : lines) {
                if (s.empty()) {
                    ++breaks;
                    continue;
                }
                const bool more = s[0] == ' ';
                if (first) out += std::string(breaks, '\n');
                else if (!breaks) out += more || prevMore ? "\n" : " ";
                else out += std::string(breaks + (more || prevMore ? 1 : 0), '\n');
                out += s;
                first = false, prevMore = more, breaks = 0;
            }
        }
        if (chomp != '-' && !out.empty()) out += '\n';
        if (chomp == '+') out += std::string(trailing, '\n');
        return Scalar(std::move(out), false);
    }

    // ---- flow: [..], {..} and quoted scalars, over lines, at m_p

    void Space()
    {
        while (m_p < m_src.size()) {
            const char c = m_src[m_p];
            if (IsSpace(c)) ++m_p;
            else if (c == '#' && (m_p == 0 || IsSpace(m_src[m_p - 1])))
                while (m_p < m_src.size() && m_src[m_p] != '\n') ++m_p;
            else break;
        }
    }
    bool At(char c) const { return m_p < m_src.size() && m_src[m_p] == c; }

    Json Flow(int depth)
    {
        if (depth > 64) FailAt("nested too deep", m_p);
        Space();
        if (m_p >= m_src.size()) FailAt("a value expected", m_p);
        const char c = m_src[m_p];
        Json v;
        if (c == '[' || c == '{') {
            const char close = c == '[' ? ']' : '}';
            ++m_p;
            v.type = c == '[' ? Json::Array : Json::Object;
            for (;;) {
                Space();
                if (At(close)) break;
                if (c == '[') {
                    v.items.push_back(Flow(depth + 1));
                } else {
                    const size_t at = m_p;
                    std::string key = At('"') || At('\'') ? Quoted() : FlowPlain(true);
                    if (v.Get(key)) FailAt("the key \"" + key + "\" twice", at);
                    Space();
                    if (!At(':')) FailAt("':' expected", m_p);
                    ++m_p;
                    Space();
                    v.members.emplace_back(std::move(key), At(',') || At('}') ? Json{} : Flow(depth + 1));
                }
                Space();
                if (At(',')) ++m_p;
                else if (!At(close)) FailAt(std::string("',' or '") + close + "' expected", m_p);
            }
            ++m_p;
        } else if (c == '"' || c == '\'') {
            v = Scalar(Quoted(), false);
        } else if (c == '&' || c == '*' || c == '!') {
            FailAt("anchors, aliases and tags are not supported", m_p);
        } else {
            v = Scalar(FlowPlain(false), true);
        }
        return v;
    }

    // A plain scalar in a flow: to a , [ ] { } or a ':' that ends a key, on its line.
    std::string FlowPlain(bool key)
    {
        const size_t from = m_p;
        while (m_p < m_src.size()) {
            const char c = m_src[m_p];
            if (c == ',' || c == '[' || c == ']' || c == '{' || c == '}' || c == '\n' || c == '\r') break;
            if (c == '#' && m_p > from && IsSpace(m_src[m_p - 1])) break;
            if (c == ':') {
                const char n = m_p + 1 < m_src.size() ? m_src[m_p + 1] : ' ';
                if (IsSpace(n) || n == ',' || n == ']' || n == '}' || n == '[' || n == '{') break;
            }
            ++m_p;
        }
        std::string s = TrimRight(m_src.substr(from, m_p - from));
        if (s.empty()) FailAt(key ? "a key expected" : "a value expected", from);
        return s;
    }

    uint32_t Hex(size_t digits)
    {
        if (m_p + digits > m_src.size()) FailAt("a short escape", m_p);
        uint32_t v = 0;
        for (size_t i = 0; i < digits; ++i) {
            const char c = m_src[m_p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else FailAt("a bad hex digit", m_p - 1);
        }
        return v;
    }

    // A '' or "" scalar at m_p; its line breaks folded (one a space, an empty line a break).
    std::string Quoted()
    {
        const char q = m_src[m_p++];
        const size_t from = m_p;
        std::string out;
        for (;;) {
            if (m_p >= m_src.size()) FailAt("an unclosed string", from - 1);
            const char c = m_src[m_p++];
            if (c == q) {
                if (q == '\'' && At('\'')) {
                    out += '\'', ++m_p;
                    continue;
                }
                return out;
            }
            if (c == '\n' || c == '\r') {
                if (c == '\r' && At('\n')) ++m_p;
                while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
                size_t breaks = 0;
                for (;;) {
                    while (At(' ') || At('\t') || At('\r')) ++m_p;
                    if (!At('\n')) break;
                    ++m_p, ++breaks;
                }
                out += breaks ? std::string(breaks, '\n') : " ";
                continue;
            }
            if (c != '\\' || q == '\'') {
                out += c;
                continue;
            }
            if (m_p >= m_src.size()) FailAt("an unclosed string", from - 1);
            switch (const char e = m_src[m_p++]) {
            case 'n': out += '\n'; break;
            case 't': case '\t': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case '0': out += '\0'; break;
            case 'a': out += '\a'; break;
            case 'e': out += '\x1b'; break;
            case ' ': out += ' '; break;
            case 'x': Utf8(Hex(2), out); break;
            case 'U': Utf8(Hex(8), out); break;
            case 'u': {
                uint32_t c16 = Hex(4);
                if (c16 >= 0xd800 && c16 < 0xdc00 && m_p + 6 <= m_src.size() && m_src[m_p] == '\\' && m_src[m_p + 1] == 'u') {
                    m_p += 2;
                    c16 = 0x10000 + ((c16 - 0xd800) << 10) + (Hex(4) - 0xdc00);
                }
                Utf8(c16, out);
                break;
            }
            case '\r':
            case '\n':   // an escaped line break: none, and the next line's indent skipped
                if (e == '\r' && At('\n')) ++m_p;
                while (At(' ') || At('\t')) ++m_p;
                break;
            case '"': case '\\': case '/': out += e; break;
            default: FailAt(std::string("an unknown escape \\") + e, m_p - 2);
            }
        }
    }
};

} // namespace

Json ParseYaml(const std::string& text)
{
    return Reader(text).Document();
}

} // namespace wrally::formats
