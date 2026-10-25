#pragma once
// RBR .lsp reader: the s-expression text the game's Tweak system reads physics definitions from (Physics/physics.lsp,
// the cars' common / surface / setup files, tyres.lsp; doc/physics-standalone-plan.md S2).
//
// Tokens: ( ) ; "strings" ; ';' comments to the end of the line ; numbers ; symbols (keys, and flag words such as
// +VALID / -HARD). A number is stored as the game stores it: Tweak::CReal (0x52f770) keeps (float)msvcr71!atof(token),
// and (float)strtod gives the same bits for every number in the vanilla files and 200,000 random strings
// (phys-atof-check). The tree is generic; binding keys to definition fields is the property tables' job.

#include <cstdint>
#include <string>
#include <vector>

namespace phys::data {

struct LspNode {
    enum Kind : uint8_t { List, Symbol, String, Number };
    Kind kind = List;
    std::string text;            // the token as written (a string without its quotes)
    float value = 0.0f;          // Number: the float the game stores
    std::vector<LspNode> items;  // List
    uint32_t line = 0;           // where it starts (1-based)

    bool IsList() const { return kind == List; }
    bool IsNumber() const { return kind == Number; }
    // In a property list ("Key value value ... Key value ..."): the node after the symbol `key`, or nullptr.
    const LspNode* After(const std::string& key) const;
    // The numbers that follow `key` up to the next non-number (a scalar: one, a vec: three).
    std::vector<float> Numbers(const std::string& key) const;
};

// The whole file as one List of its top-level forms. Throws std::runtime_error (with the line) on an unclosed '(' or
// string. A stray ')' after the last form ends the file (both vanilla tyres.lsp have one); the root's text notes it.
LspNode ParseLsp(const std::string& text);

// A file from disk, or a member of a zip (physics.rbz, stored or deflated entries).
std::string ReadFileText(const std::string& path);
std::string ReadZipMember(const std::string& zipPath, const std::string& member);
std::vector<std::string> ListZip(const std::string& zipPath);

} // namespace phys::data
