// catalog-check: what the Quick rally menu offers (assets::Catalog) from an RBR folder, checked against what it must
// hold - every RBR stage, the ones no .ngg draws imported from their .lbs with their weathers drawn, but no protected
// (encrypted) one, and the TrackSettings.ini weathers of a map under 10 found under their two-digit names ("04E_...").
//
//   catalog-check <RBR folder> [<.ngg folders>]
//
// Prints the counts and "catalog-check: all good", or what is wrong and exits 1.

#include <cstdio>
#include <cstring>
#include <string>

#include "app/settings.h"
#include "assets/catalog.h"
#include "formats/lbs_geometry.h"

namespace {

const wrally::assets::Stage* Find(const wrally::assets::Catalog& c, const char* stage)
{
    for (const wrally::assets::Stage& s : c.stages)
        if (!_stricmp(s.stage.c_str(), stage)) return &s;
    return nullptr;
}

int Fail(const std::string& why)
{
    std::printf("catalog-check: %s\n", why.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) return std::printf("usage: catalog-check <RBR folder> [<.ngg folders>]\n"), 2;
    wrally::app::Settings s;
    s.rbrPath = argv[1];
    if (argc > 2) s.nggFolders = argv[2];
    wrally::assets::Catalog c;
    if (const std::string e = c.Scan(s); !e.empty()) return Fail(e);

    size_t imported = 0, drawn = 0;
    for (const wrally::assets::Stage& st : c.stages) {
        imported += st.imported;
        drawn += st.weathersDrawn;
        if (st.imported && !st.weathersDrawn) return Fail(st.stage + ": imported, but its weathers are not drawn");
    }
    std::printf("%zu stages: %zu from .ngg files, %zu imported from .lbs; %zu with drawn weathers; %zu protected (encrypted, "
                "not offered); %zu cars\n", c.stages.size(), c.stages.size() - imported, imported, drawn,
                c.protectedStages.size(), c.cars.size());
    // No encrypted stage offered for import (it would fail when picked, and must not be imported anyway).
    for (const wrally::assets::Stage& st : c.stages)
        if (st.imported && wrally::formats::LbsEncryptedVersion(st.ngg)) return Fail(st.stage + ": encrypted, but offered");

    // A map under 10, imported (no .ngg draws it in a stock RBR folder): its evening weathers, two-digit names.
    const wrally::assets::Stage* four = Find(c, "track-4_E");
    if (!four) return Fail("track-4_E is not offered");
    if (four->weathers.empty()) return Fail("track-4_E has no weathers (a \"4E_\" prefix finds none)");
    for (const std::string& w : four->weathers)
        if (_strnicmp(w.c_str(), "04E_", 4) && _strnicmp(w.c_str(), "04O_E_", 6)) return Fail("track-4_E offers " + w);
    std::printf("track-4_E: %s, %zu weathers (%s ...)\n", four->imported ? "imported" : "from an .ngg", four->weathers.size(),
                four->weathers[0].c_str());
    std::printf("catalog-check: all good\n");
    return 0;
}
