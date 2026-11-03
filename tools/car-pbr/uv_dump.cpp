// car-uv-dump <car .sgc> [surface]: the car's exterior mesh groups (assets::ReadCarModel: as W-Rally draws them, the
// wheels of `surface`, gravel by default) as UV triangles, one line each -
//
//     tri <texture file> <shader class> <mesh> u0 v0 u1 v1 u2 v2
//
// <shader class> "-" where <car>.ini gives none; a group without a texture file is left out. make_pbr.py reads it.

#include <cstdio>
#include <exception>
#include <string>

#include "assets/car_model.h"

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: car-uv-dump <car .sgc> [surface]\n");
        return 2;
    }
    try {
        const wrally::assets::CarModel model = wrally::assets::ReadCarModel(argv[1], argc > 2 ? argv[2] : "gravel");
        for (const wrally::assets::CarPart& part : model.parts) {
            std::string file;
            for (const wrally::assets::CarTexture& t : model.materials)
                if (t.material == part.material) file = t.file;
            if (file.empty()) continue;
            const std::string shader = part.shader.empty() ? "-" : part.shader;
            for (size_t i = 0; i + 2 < part.indices.size(); i += 3) {
                std::printf("tri %s %s %s", file.c_str(), shader.c_str(), part.name.c_str());
                for (int k = 0; k < 3; ++k) {
                    const wrally::assets::CarVertex& v = part.vertices.at(part.indices[i + k]);
                    std::printf(" %.6f %.6f", v.uv[0], v.uv[1]);
                }
                std::printf("\n");
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "car-uv-dump: %s\n", e.what());
        return 1;
    }
    return 0;
}
