#pragma once
// wrally::assets::CarRegister - what W-Rally knows of each car the catalog found (assets/catalog.h): its friendly name,
// maker, class, drive, power, weight, physics - in layers, a later one winning field by field:
//
//   1. RBR's files     Cars.ini's CarName, the physics the catalog picked (its own, its slot's, the base)
//   2. the server      W-Rally's car list (hardcoded until there is a server: car_register.cpp kServerCars)
//   3. car.yaml        the car's own manifest, for a mod or a fix: the old place in the RBR folder
//                      (rbr/Cars/<folder>/car.yaml), then its newest version's in the content layers
//                      (cars/<id>/<version>/car.yaml, a downloaded or W-Rally's - docs/content-layers.md);
//                      car.yml or an older car.json where there is no car.yaml
//
// A car neither the server nor a car.yaml knows is shown, but not usable by W-Rally (CarInfo::known): nothing says
// what it is or how it drives.
//
// car.yaml (every key optional):
//   name: Hyundai i20 R5
//   maker: Hyundai
//   year: 2016
//   class: R5
//   drive: 4WD
//   power_hp: 280
//   weight_kg: 1230
//   physics: s_i2003            # a physics variant's id, or a folder: Cars/i20_R5/physics/

#include <string>
#include <vector>

#include "wrally/vfs/vfs.h"

namespace wrally::assets {

struct Car;

struct CarInfo {
    std::string id;                 // the catalog's Car::folder (an RBR car's folder, else its id)
    std::string name;               // friendly: "Hyundai i20 R5"
    std::string maker;
    int year = 0;                   // 0 unknown
    std::string carClass;           // "WRC", "Group N", "Super 1600", "R5", "S2000", "Group A", "Kit car" ("" none)
    std::string drive;              // "4WD", "FWD", "RWD" ("" unknown)
    float powerHp = 0, weightKg = 0;   // 0 unknown
    std::string physics;            // car.yaml's: a physics.rbz folder, or Cars/<folder>/physics/ ("" the catalog's pick)
    bool known = false;             // the server or a car.yaml knows it: usable by W-Rally
    std::string from;               // the layers that said something: "RBR", "RBR + server", "RBR + server + car.yaml (downloaded)"
    std::string error;              // its car.yaml's, if it could not be read ("" none)
};

class CarRegister {
public:
    // The register for the catalog's cars, their manifests from `content`.
    void Build(const vfs::Vfs& content, const std::vector<Car>& cars);
    const CarInfo* Find(const std::string& id) const;
    // The classes the known cars have, in the register's order (WRC first).
    std::vector<std::string> Classes() const;

private:
    std::vector<CarInfo> m_cars;
};

} // namespace wrally::assets
