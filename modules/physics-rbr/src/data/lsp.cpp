#include "lsp.h"
#include "files.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "inflate.h"

namespace phys::data {

namespace {

bool IsNumberToken(const std::string& t, float& out)
{
    const char* s = t.c_str();
    const char* p = s;
    if (*p == '+' || *p == '-') ++p;
    if (!std::isdigit(static_cast<unsigned char>(*p)) && !(*p == '.' && std::isdigit(static_cast<unsigned char>(p[1])))) return false;
    char* end = nullptr;
    const double d = std::strtod(s, &end);
    if (*end != 0) return false;
    out = static_cast<float>(d);   // as Tweak::CReal: (float)atof (phys-atof-check)
    return true;
}

class Parser {
public:
    explicit Parser(const std::string& t) : t_(t) {}

    LspNode File()
    {
        LspNode root;
        root.line = 1;
        for (;;) {
            Skip();
            if (at_ == t_.size()) return root;
            if (t_[at_] == ')') {   // a stray ')' after the last form (both vanilla tyres.lsp end so): the rest ignored
                root.text = "stray ')' at line " + std::to_string(line_);
                return root;
            }
            root.items.push_back(Item());
        }
    }

private:
    [[noreturn]] void Fail(const char* what) const
    {
        throw std::runtime_error(std::string("lsp line ") + std::to_string(line_) + ": " + what);
    }
    void Skip()
    {
        while (at_ < t_.size()) {
            const char c = t_[at_];
            if (c == '\n') {
                ++line_, ++at_;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                ++at_;
            } else if (c == ';') {
                while (at_ < t_.size() && t_[at_] != '\n') ++at_;
            } else {
                return;
            }
        }
    }
    LspNode Item()
    {
        LspNode n;
        n.line = line_;
        const char c = t_[at_];
        if (c == '(') {
            ++at_;
            n.kind = LspNode::List;
            for (;;) {
                Skip();
                if (at_ == t_.size()) Fail("unclosed '('");
                if (t_[at_] == ')') {
                    ++at_;
                    return n;
                }
                n.items.push_back(Item());
            }
        }
        if (c == '"') {
            const size_t end = t_.find('"', at_ + 1);
            if (end == std::string::npos) Fail("unterminated string");
            n.kind = LspNode::String;
            n.text = t_.substr(at_ + 1, end - at_ - 1);
            for (char ch : n.text) line_ += ch == '\n';
            at_ = end + 1;
            return n;
        }
        const size_t start = at_;
        while (at_ < t_.size() && !std::isspace(static_cast<unsigned char>(t_[at_])) && t_[at_] != '(' && t_[at_] != ')' && t_[at_] != '"' &&
               t_[at_] != ';')
            ++at_;
        n.text = t_.substr(start, at_ - start);
        n.kind = IsNumberToken(n.text, n.value) ? LspNode::Number : LspNode::Symbol;
        return n;
    }

    const std::string& t_;
    size_t at_ = 0;
    uint32_t line_ = 1;
};

uint32_t U32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

bool SameName(const std::string& a, const std::string& b)   // case-insensitive, '/' == '\'
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i]))), y = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
        if (x != y) return false;
    }
    return true;
}

} // namespace

const LspNode* LspNode::After(const std::string& key) const
{
    for (size_t i = 0; i + 1 < items.size(); ++i)
        if (items[i].kind == Symbol && items[i].text == key) return &items[i + 1];
    return nullptr;
}

std::vector<float> LspNode::Numbers(const std::string& key) const
{
    std::vector<float> out;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].kind == Symbol && items[i].text == key) {
            for (size_t k = i + 1; k < items.size() && items[k].kind == Number; ++k) out.push_back(items[k].value);
            break;
        }
    return out;
}

LspNode ParseLsp(const std::string& text) { return Parser(text).File(); }

std::string ReadFileText(const std::string& path)
{
    std::vector<uint8_t> d;
    if (!ReadWholeFile(path, d)) throw std::runtime_error("cannot open " + path);
    return std::string(d.begin(), d.end());
}

namespace {
// Calls fn(name, method, csize, usize, data offset) per central-directory entry until it returns true.
template <class Fn> void ZipWalk(const std::string& zipPath, const std::string& z, Fn fn)
{
    const auto* d = reinterpret_cast<const uint8_t*>(z.data());
    // The end of central directory record, searched from the end (after it: a comment of up to 64 KB).
    size_t eocd = std::string::npos;
    for (size_t i = z.size() >= 22 ? z.size() - 22 : 0;; --i) {
        if (U32(d + i) == 0x06054b50) {
            eocd = i;
            break;
        }
        if (i == 0 || z.size() - i > 22 + 0xffff) break;
    }
    if (eocd == std::string::npos) throw std::runtime_error(zipPath + ": not a zip");
    const uint16_t entries = U16(d + eocd + 10);
    size_t at = U32(d + eocd + 16);
    for (uint16_t e = 0; e < entries; ++e) {
        if (at + 46 > z.size() || U32(d + at) != 0x02014b50) throw std::runtime_error(zipPath + ": bad central directory");
        const uint16_t method = U16(d + at + 10);
        const uint32_t csize = U32(d + at + 20), usize = U32(d + at + 24);
        const uint16_t nlen = U16(d + at + 28), xlen = U16(d + at + 30), clen = U16(d + at + 32);
        const uint32_t local = U32(d + at + 42);
        const std::string name(z.data() + at + 46, nlen);
        at += 46 + nlen + xlen + clen;
        if (local + 30 > z.size() || U32(d + local) != 0x04034b50) throw std::runtime_error(zipPath + ": bad local header");
        const size_t data = local + 30 + U16(d + local + 26) + U16(d + local + 28);
        if (data + csize > z.size()) throw std::runtime_error(zipPath + ": truncated member");
        if (fn(name, method, csize, usize, data)) return;
    }
}
} // namespace

std::string ReadZipMember(const std::string& zipPath, const std::string& member)
{
    const std::string z = ReadFileText(zipPath);
    std::string result;
    bool found = false;
    ZipWalk(zipPath, z, [&](const std::string& name, uint16_t method, uint32_t csize, uint32_t usize, size_t data) {
        if (!SameName(name, member)) return false;
        found = true;
        if (method == 0) {
            result = z.substr(data, csize);
            return true;
        }
        if (method != 8) throw std::runtime_error(zipPath + ": unsupported method for " + name);
        std::vector<uint8_t> out;
        Inflate(reinterpret_cast<const uint8_t*>(z.data()) + data, csize, out, usize);
        if (out.size() != usize) throw std::runtime_error(zipPath + ": size mismatch for " + name);
        result.assign(out.begin(), out.end());
        return true;
    });
    if (!found) throw std::runtime_error(zipPath + ": no member " + member);
    return result;
}

std::vector<std::string> ListZip(const std::string& zipPath)
{
    const std::string z = ReadFileText(zipPath);
    std::vector<std::string> names;
    ZipWalk(zipPath, z, [&](const std::string& name, uint16_t, uint32_t, uint32_t, size_t) {
        names.push_back(name);
        return false;
    });
    return names;
}


} // namespace phys::data
