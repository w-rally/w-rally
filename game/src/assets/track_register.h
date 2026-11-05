#pragma once
// wrally::assets::TrackRegister - what W-Rally knows of each track (an RBR map: its stages are the map's times of day,
// track-41_M, track-41_O, ...): its name, country, surface, length - in layers, a later one winning field by field:
//
//   1. RBR's files     Maps/Tracks.ini's [Map<n>] StageName and SplashScreen; the surface as the game picks it
//                      (rules::StageSurface); the country where its particles say one (ps_british, ps_usa, ps_japan,
//                      ps_australia, ps_montblanc: France)
//   2. the server      W-Rally's track list (hardcoded until there is a server: track_register.cpp kServerTracks)
//   3. track.yaml      the track's own manifest, for a mod or a fix: its newest version's in the content layers
//                      (tracks/<id>/<version>/track.yaml, a downloaded or W-Rally's - docs/content-layers.md;
//                      track.yml or an older track.json where there is no track.yaml)
//
// track.yaml (every key optional but the id and version the layers need):
//   id: fr_mont_gaudin
//   version: 1.0.0
//   name: Mont Gaudin
//   country: France
//   surface: tarmac
//   length_km: 10.2
//   splash: Textures\Splash\load_23.dds     # (a plain YAML value: no escapes, one \ is one)

#include <string>
#include <vector>

#include "wrally/vfs/vfs.h"

namespace wrally::assets {

struct Stage;

struct TrackInfo {
    int map = -1;
    std::string name;               // "" unknown (the menus show the stage's file name)
    std::string country;            // "" unknown
    std::string surface;            // "gravel", "tarmac", "snow"
    float lengthKm = 0;             // 0 unknown
    // Its splash picture (assets/splash.h): a path relative to the RBR folder, or an http(s) URL the server serves it
    // at ("" none). Tracks.ini's SplashScreen for a community track; the server's list (the original stages); its json.
    std::string splash;
    std::string from;               // the layers that said something: "RBR", "RBR + server", "RBR + track.yaml (downloaded)"
    std::string error;              // its json's, if it could not be read ("" none)
};

class TrackRegister {
public:
    // The register for the catalog's stages' maps, their manifests from `content`.
    void Build(const vfs::Vfs& content, const std::vector<Stage>& stages);
    const TrackInfo* Find(int map) const;
    // The countries the tracks are in, sorted ("" left out).
    std::vector<std::string> Countries() const;

private:
    std::vector<TrackInfo> m_tracks;
};

} // namespace wrally::assets
