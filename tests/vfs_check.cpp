// vfs-check: wrally::vfs (libs/vfs, docs/content-layers.md) on a made-up downloaded layer, W-Rally layer and RBR folder
// (with a physics.rbz of its own), written into a scratch folder; then, given one, on a real RBR folder.
//
//   vfs-check <scratch folder> [<RBR folder>]
//
// Prints "vfs-check: all good", or each check that failed and exits 1.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "wrally/vfs/vfs.h"

namespace fs = std::filesystem;
using namespace wrally::vfs;

namespace {

int g_failed = 0;

void Check(bool ok, const std::string& what)
{
    if (!ok) std::printf("FAILED: %s\n", what.c_str()), ++g_failed;
}

void Write(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string Text(const Vfs& vfs, const std::string& path)
{
    std::vector<uint8_t> b;
    return vfs.Read(path, b) ? std::string(b.begin(), b.end()) : "<none>";
}

std::string Text(const Item& item, const std::string& rel)
{
    std::vector<uint8_t> b;
    const auto r = item.Find(rel);
    return r && ReadResolved(*r, b) ? std::string(b.begin(), b.end()) : "<none>";
}

std::string Joined(const std::vector<std::string>& v)
{
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : ",") + x;
    return s;
}

// A zip of stored members, as RBR's physics.rbz is read (phys::data::ReadZip).
uint32_t Crc32(const std::string& d)
{
    uint32_t c = 0xffffffffu;
    for (unsigned char b : d) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void WriteZip(const fs::path& p, const std::vector<std::pair<std::string, std::string>>& members)
{
    std::string out, central;
    auto u16 = [](std::string& s, uint32_t v) { s += char(v & 0xff), s += char(v >> 8 & 0xff); };
    auto u32 = [&](std::string& s, uint32_t v) { u16(s, v & 0xffff), u16(s, v >> 16); };
    for (const auto& [name, data] : members) {
        const uint32_t offset = static_cast<uint32_t>(out.size()), crc = Crc32(data), size = static_cast<uint32_t>(data.size());
        u32(out, 0x04034b50), u16(out, 20), u16(out, 0), u16(out, 0), u16(out, 0), u16(out, 0);
        u32(out, crc), u32(out, size), u32(out, size), u16(out, uint32_t(name.size())), u16(out, 0);
        out += name + data;
        u32(central, 0x02014b50), u16(central, 20), u16(central, 20), u16(central, 0), u16(central, 0), u16(central, 0);
        u16(central, 0), u32(central, crc), u32(central, size), u32(central, size), u16(central, uint32_t(name.size()));
        u16(central, 0), u16(central, 0), u16(central, 0), u16(central, 0), u32(central, 0), u32(central, offset);
        central += name;
    }
    const uint32_t at = static_cast<uint32_t>(out.size());
    out += central;
    u32(out, 0x06054b50), u16(out, 0), u16(out, 0), u16(out, uint32_t(members.size())), u16(out, uint32_t(members.size()));
    u32(out, static_cast<uint32_t>(central.size())), u32(out, at), u16(out, 0);
    Write(p, out);
}

void Names()
{
    Check(NormalPath("\\Cars//foo\\.\\bar/") == "Cars/foo/bar", "NormalPath");
    Check(NormalPath("cars/../x").empty(), "NormalPath refuses ..");
    Check(ValidId("cz_ploskovice") && ValidId("s_i2003") && !ValidId("41") && !ValidId("Cz") && !ValidId("a-b"), "ValidId");
    Check(ValidVersion("1") && ValidVersion("1.2.3") && ValidVersion("rbr") && !ValidVersion("1..2") && !ValidVersion("v1"),
          "ValidVersion");
    Check(CompareVersions("rbr", "0.0.1") < 0 && CompareVersions("1.10", "1.9") > 0 && CompareVersions("1.0", "1.0.0") == 0,
          "CompareVersions");
    Check(Slug("LANCER_EVO_9_GrN") == "lancer_evo_9_grn" && Slug("207_s2000.old") == "207_s2000_old", "Slug");
}

void MadeUp(const fs::path& root)
{
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path dl = root / "downloaded", wr = root / "w-rally", rbr = root / "rbr";

    // RBR: two maps (4 and 41, whose names start alike) and one Tracks.ini lists with no file here (77), a slot car with physics.rbz's physics, a car with its own
    // loose physics, a car folder without an .sgc.
    Write(rbr / "Maps/Tracks.ini", "[Map04]\nTrackName = \"Maps\\track-4\"\n[Map41]\nTrackName\t= \"Maps\\track-41\" ; c\n"
                                   "[Map77]\nTrackName = \"Maps\\track-77\"\n");
    Write(rbr / "Maps/track-4_E.trk", "trk 4");
    Write(rbr / "Maps/track-41_M.trk", "trk 41 rbr");
    Write(rbr / "Maps/track-41_M.lbs", "lbs 41 rbr");
    Write(rbr / "Maps/track-41_M_textures.rbz", "tex 41");
    Write(rbr / "Maps/track-12_O.trk", "trk 12, in no Tracks.ini");
    Write(rbr / "Maps/track-45_N.trk", "trk 45, 41 reversed");
    Write(rbr / "Cars/Cars.ini", "[Car05]\nFileName = \"Cars\\Impreza03\\imp.sgc\"\nCarName = \"Subaru Impreza 2003\"\n");
    Write(rbr / "Cars/Impreza03/imp.sgc", "sgc rbr");
    Write(rbr / "Cars/Impreza03/textures/body.dds", "body rbr");
    Write(rbr / "Cars/Own_Car/own.sgc", "sgc own");
    Write(rbr / "Cars/Own_Car/physics/common.lsp", "own common");
    Write(rbr / "Cars/NoModel/readme.txt", "x");
    WriteZip(rbr / "physics.rbz", {{"Physics/physics.lsp", "global defs"},
                                   {"Physics/michelin/tyres.lsp", "michelin tyres"},
                                   {"Physics/s_i2003/common.lsp", "i2003 common"},
                                   {"Physics/s_i2003/setups/d_gravel.lsp", "i2003 gravel setup"}});

    // W-Rally: the track table (map 41 and its reverse 45 one track, 4 not named; 77 in the older one-a-line form), a
    // track of its own, a car version (an older car.json).
    Write(wr / "rbr-tracks.yaml", "# track: its stages\ncz_test:\n  41: test\n  45: test_ii\n77: BadId\n");
    Write(wr / "tracks/fr_own/1.0.0/track.yaml", "id: fr_own\nversion: 1.0.0\n");
    Write(wr / "tracks/fr_own/1.0.0/a.trk", "a 1.0.0");
    Write(wr / "tracks/fr_own/1.0.0/b.trk", "b 1.0.0");
    Write(wr / "cars/impreza03/1.0.0/car.json", R"({"id": "impreza03", "version": "1.0.0"})");
    Write(wr / "cars/impreza03/1.0.0/imp.sgc", "sgc w-rally 1.0.0");
    Write(wr / "video/menu.mp4", "video");
    Write(wr / "tracks/Bad-Name/1.0.0/track.yaml", "{}");

    // Downloaded: patches over RBR's car and over the track, a newer track version, the same version as W-Rally's
    // (the downloaded one wins whole), a physics variant, broken chains.
    Write(dl / "cars/impreza03/1.1.0/car.yaml", "# a patch\nid: impreza03\nversion: 1.1.0\nbase: rbr\n");
    Write(dl / "cars/impreza03/1.1.0/car.json", R"({"id": "impreza03", "version": "9.9.9"})");   // left out: car.yaml is
    Write(dl / "cars/impreza03/1.1.0/textures/body_orm.dds", "orm 1.1.0");
    Write(dl / "cars/impreza03/physics/gravel_pro/2.0.0/physics.yml", "id: gravel_pro\nversion: 2.0.0\nname: Gravel pro\n");
    Write(dl / "cars/impreza03/physics/gravel_pro/2.0.0/common.lsp", "pro common");
    Write(dl / "tracks/fr_own/1.1.0/track.yaml", "id: fr_own\nversion: 1.1.0\nbase: 1.0.0\n");
    Write(dl / "tracks/fr_own/1.1.0/b.trk", "b 1.1.0");
    Write(dl / "tracks/fr_own/1.0.0/track.json", R"({"id": "fr_own", "version": "1.0.0"})");   // shadows W-Rally's 1.0.0
    Write(dl / "tracks/fr_own/1.0.0/a.trk", "a 1.0.0 downloaded");
    Write(dl / "tracks/cz_test/1.2.0/track.yaml", "id: cz_test\nversion: 1.2.0\nbase: rbr\n");
    Write(dl / "tracks/cz_test/1.2.0/cz_test.ngg", "ngg");
    Write(dl / "tracks/lost/1.0.0/track.yaml", "version: 1.0.0\nbase: 0.9\n");
    Write(dl / "tracks/round/1.0.0/track.yaml", "base: 2.0.0\n");
    Write(dl / "tracks/round/2.0.0/track.yaml", "base: 1.0.0\n");
    Write(dl / "tracks/broken/1.0.0/track.yaml", "name: a\n  bad: indent\n");

    const Vfs vfs({{dl, "downloaded"}, {wr, "w-rally"}}, rbr);

    // versions, newest first, RBR's the oldest
    Check(Joined(vfs.Versions(Kind::Car, "impreza03")) == "1.1.0,1.0.0,rbr", "car versions: " + Joined(vfs.Versions(Kind::Car, "impreza03")));
    Check(Joined(vfs.Versions(Kind::Track, "fr_own")) == "1.1.0,1.0.0", "track versions");
    Check(Joined(vfs.Ids(Kind::Track)) == "broken,cz_test,fr_own,lost,rbr_12,rbr_4,round", "track ids: " + Joined(vfs.Ids(Kind::Track)));
    Check(Joined(vfs.Ids(Kind::Car)) == "impreza03,own_car", "car ids: " + Joined(vfs.Ids(Kind::Car)));

    // a patch over RBR's car: its own files, then RBR's
    std::string error;
    const auto car = vfs.Open({Kind::Car, "impreza03"}, &error);
    Check(car.has_value(), "open the newest car: " + error);
    if (car) {
        Check(car->Ref().version == "1.1.0" && Joined(car->Chain()) == "1.1.0,rbr", "car chain " + Joined(car->Chain()));
        Check(Text(*car, "textures/body_orm.dds") == "orm 1.1.0" && car->Source("textures/body_orm.dds") == "downloaded", "patch's own file");
        Check(Text(*car, "imp.sgc") == "sgc rbr" && car->Source("IMP.SGC") == "rbr", "RBR's file under the patch");
        Check(car->Manifest().Str("version") == "1.1.0" && car->ManifestFile() == "car.yaml", "the version's own manifest, car.yaml first");
        Check(Text(*car, "car.json").find("base: rbr") != std::string::npos, "any manifest name finds the manifest");
        Check(Joined(car->Files()) == "car.yaml,imp.sgc,textures/body.dds,textures/body_orm.dds", "car files: " + Joined(car->Files()));
    }
    const auto car100 = vfs.Open({Kind::Car, "impreza03", "", "1.0.0"});
    Check(car100 && car100->ManifestFile() == "car.json" && car100->Manifest().Str("version") == "1.0.0", "an older car.json still read");
    Check(vfs.Log().find("car.json: left out, car.yaml is the manifest") != std::string::npos, "a second manifest logged");
    Check(vfs.Log().find("broken/1.0.0/track.yaml: bad indentation (line 2)") != std::string::npos, "a bad manifest logged with its line");
    const auto pro = vfs.Open({Kind::Physics, "gravel_pro", "impreza03"});
    Check(pro && pro->ManifestFile() == "physics.yml" && pro->Manifest().Str("name") == "Gravel pro", "a .yml manifest");
    {
    }
    const auto rbrCar = vfs.Open({Kind::Car, "impreza03", "", "rbr"});
    Check(rbrCar && rbrCar->Manifest().Str("name") == "Subaru Impreza 2003" && rbrCar->Manifest().Str("version") == "rbr",
          "RBR car's manifest, made in memory");
    Check(rbrCar && Text(*rbrCar, "car.yaml").find("\"slot\": 5") != std::string::npos, "the made manifest read as a file");
    Check(vfs.RbrCarFolder("impreza03") == "Impreza03", "RBR car folder");

    // a track's chain within the layers; the same version in two layers is the higher one's, whole
    const auto own = vfs.Open({Kind::Track, "fr_own"});
    Check(own && Text(*own, "b.trk") == "b 1.1.0" && Text(*own, "a.trk") == "a 1.0.0 downloaded", "track chain over layers");
    Check(own && own->Source("a.trk") == "downloaded", "same version: the higher layer's");
    const auto own100 = vfs.Open({Kind::Track, "fr_own", "", "1.0"});
    Check(own100 && Text(*own100, "b.trk") == "<none>", "1.0 is 1.0.0, without W-Rally's files (downloaded's whole)");

    // broken chains
    Check(!vfs.Open({Kind::Track, "lost"}, &error) && error.find("base 0.9 is not here") != std::string::npos, "missing base: " + error);
    Check(!vfs.Open({Kind::Track, "round"}, &error) && error.find("go round") != std::string::npos, "base cycle: " + error);
    Check(!vfs.Open({Kind::Track, "fr_own", "", "3.0.0"}, &error) && error.find("1.1.0, 1.0.0") != std::string::npos,
          "absent version: " + error);

    // RBR's tracks: the table's id, else rbr_<n>; a map's files only, never another map's sharing its first digits
    Check(vfs.TrackOfMap(41) == "cz_test" && vfs.MapOfTrack("cz_test") == 41 && vfs.TrackOfMap(4) == "rbr_4", "map table");
    // a reversed stage is a stage of the same track: one item, both maps' files, the main map the table's first
    Check(vfs.TrackOfMap(45) == "cz_test" && vfs.MapsOfTrack("cz_test") == std::vector<int>{41, 45} &&
              vfs.StageOfMap(41) == "test" && vfs.StageOfMap(45) == "test_ii" && vfs.StageOfMap(4) == "rbr_4",
          "a track's stages: " + vfs.StageOfMap(41) + ", " + vfs.StageOfMap(45));
    Check(Text(vfs, "tracks/cz_test/rbr/track-45_N.trk") == "trk 45, 41 reversed", "the reversed stage's file in the track");
    {
        const auto rt = vfs.Open({Kind::Track, "cz_test", "", "rbr"});
        const wrally::formats::Json* st = rt ? rt->Manifest().Get("stages") : nullptr;
        Check(st && st->items.size() == 2 && st->items[1].Str("id") == "test_ii" && st->items[1].Num("rbr") == 45,
              "track.yaml lists its stages");
    }
    Check(Text(vfs, "tracks/cz_test/rbr/track-41_M.trk") == "trk 41 rbr", "RBR track file by its item path");
    const auto t4 = vfs.Open({Kind::Track, "rbr_4"});
    Check(t4 && Joined(t4->Files()) == "track-4_E.trk,track.yaml", "map 4's files: " + (t4 ? Joined(t4->Files()) : ""));
    const auto t41 = vfs.Open({Kind::Track, "cz_test"});
    Check(t41 && Text(*t41, "cz_test.ngg") == "ngg" && Text(*t41, "track-41_M.lbs") == "lbs 41 rbr", "a patch over an RBR track");
    Check(t41 && t41->Manifest().Num("rbr", -1) == -1 && vfs.Open({Kind::Track, "cz_test", "", "rbr"})->Manifest().Num("rbr") == 41,
          "RBR track manifest: its map");
    Check(vfs.Log().find("map 4 is in no rbr-tracks.yaml") != std::string::npos, "unknown map logged");
    Check(vfs.Log().find("\"BadId\" is not an id") != std::string::npos, "bad table id logged");
    Check(vfs.Log().find("Bad-Name: not an id") != std::string::npos, "bad id folder logged");

    // physics: physics.rbz's slot folder and base, a car's loose folder, a downloaded variant
    Check(Joined(vfs.Ids(Kind::Physics, "impreza03")) == "gravel_pro,rbr", "variants: " + Joined(vfs.Ids(Kind::Physics, "impreza03")));
    Check(Text(vfs, "cars/impreza03/physics/rbr/rbr/common.lsp") == "i2003 common", "slot physics from physics.rbz");
    Check(Text(vfs, "cars/impreza03/physics/rbr/rbr/Setups/D_Gravel.lsp") == "i2003 gravel setup", "member, any case");
    Check(Text(vfs, "physics/rbr/rbr/michelin/tyres.lsp") == "michelin tyres", "physics base from physics.rbz");
    Check(Text(vfs, "cars/own_car/physics/rbr/rbr/common.lsp") == "own common", "a car's own loose physics");
    Check(Text(vfs, "cars/impreza03/physics/gravel_pro/2.0.0/common.lsp") == "pro common", "a downloaded variant");
    const auto rbrPhysics = vfs.Open({Kind::Physics, "rbr", "impreza03"});
    Check(rbrPhysics && Joined(rbrPhysics->Files()) == "common.lsp,physics.yaml,setups/d_gravel.lsp" &&
              rbrPhysics->Manifest().Str("format") == "rbr-lsp", "RBR variant's files");

    // raw, plain, listing, writing
    Check(Text(vfs, "rbr/Maps/Tracks.ini").find("[Map41]") != std::string::npos, "raw RBR path");
    Check(Text(vfs, "video/menu.mp4") == "video" && vfs.Find("video/menu.mp4")->source == "w-rally", "plain file");
    Check(!vfs.Find("rbr/physics.rbz:Physics/physics.lsp"), "no archive member paths");
    Check(Joined(vfs.List("cars/impreza03")) == "1.0.0/,1.1.0/,physics/,rbr/", "List of a car: " + Joined(vfs.List("cars/impreza03")));
    Check(Joined(vfs.List("cars/impreza03/1.1.0/textures")) == "body.dds,body_orm.dds", "List in a version");
    Check(Joined(vfs.List("")) == "cars/,rbr-tracks.yaml,rbr/,tracks/,video/", "List of the root: " + Joined(vfs.List("")));
    Check(vfs.WritePath("cars/impreza03/2.0.0/car.yaml") == dl / "cars/impreza03/2.0.0/car.yaml", "writes go to layer 1");
    Check(vfs.WritePath("rbr/Maps/x").empty() && vfs.WritePath("../x").empty(), "never into RBR, never out");

    // no RBR: standalone
    const Vfs alone({{dl, "downloaded"}, {wr, "w-rally"}}, "");
    Check(Joined(alone.Versions(Kind::Car, "impreza03")) == "1.1.0,1.0.0" && !alone.Open({Kind::Car, "impreza03"}),
          "without RBR: a patch over RBR cannot open");
    Check(alone.Open({Kind::Car, "impreza03", "", "1.0.0"}).has_value() && !alone.Find("rbr/Maps/Tracks.ini"), "without RBR");
}

void Real(const fs::path& rbr)
{
    const Vfs vfs({}, rbr);
    const std::string track = vfs.TrackOfMap(41);
    Check(track == "rbr_41", "real: map 41 " + track);
    Check(vfs.Find("tracks/rbr_41/rbr/track-41_M.trk").has_value(), "real: stage 41's .trk");
    Check(vfs.Find("physics/rbr/rbr/physics.lsp").has_value() && vfs.Find("physics/rbr/rbr/michelin/tyres.lsp").has_value(),
          "real: the physics base");
    size_t drivable = 0;
    for (const std::string& car : vfs.Ids(Kind::Car)) {
        const auto p = vfs.Open({Kind::Physics, "rbr", car});
        drivable += p && p->Find("common.lsp").has_value();
    }
    std::printf("real RBR %s: %zu tracks, %zu cars, %zu with physics\n", rbr.string().c_str(), vfs.Ids(Kind::Track).size(),
                vfs.Ids(Kind::Car).size(), drivable);
    Check(drivable >= 8, "real: the slot cars' physics");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) return std::printf("usage: vfs-check <scratch folder> [<RBR folder>]\n"), 2;
    Names();
    MadeUp(argv[1]);
    if (argc > 2) Real(argv[2]);
    if (g_failed) return std::printf("vfs-check: %d failed\n", g_failed), 1;
    std::printf("vfs-check: all good\n");
    return 0;
}
