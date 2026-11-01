#include "assets/car_register.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "assets/catalog.h"
#include "formats/json.h"

namespace wrally::assets {

namespace {

// The server's car list, until there is a server: by the car's folder (any case). Only what is sure: a field left
// empty (0) is unknown, for a car.json to give.
struct ServerCar {
    const char* id;
    const char* name;
    const char* maker;
    int year;
    const char* carClass;
    const char* drive;
};
const ServerCar kServerCars[] = {
    {"XSARA", "Citro\xC3\xABn Xsara WRC", "Citro\xC3\xABn", 0, "WRC", "4WD"},
    {"XSARA.CHAMP", "Citro\xC3\xABn Xsara WRC", "Citro\xC3\xABn", 0, "WRC", "4WD"},
    {"XSARA2005", "Citro\xC3\xABn Xsara WRC 2005", "Citro\xC3\xABn", 2005, "WRC", "4WD"},
    {"ACCENT", "Hyundai Accent WRC", "Hyundai", 0, "WRC", "4WD"},
    {"ZR", "MG ZR", "MG", 0, "Super 1600", "FWD"},
    {"LANCER_EVO_VII", "Mitsubishi Lancer Evo VII", "Mitsubishi", 0, "Group N", "4WD"},
    {"LANCER_EVO_VII.CHAMP", "Mitsubishi Lancer Evo VII", "Mitsubishi", 0, "Group N", "4WD"},
    {"LANCER_EVO_VII_GrN", "Mitsubishi Lancer Evo VII", "Mitsubishi", 0, "Group N", "4WD"},
    {"LANCER_EVO_8_GrN", "Mitsubishi Lancer Evo VIII", "Mitsubishi", 0, "Group N", "4WD"},
    {"LANCER_EVO_9_GrN", "Mitsubishi Lancer Evo IX", "Mitsubishi", 0, "Group N", "4WD"},
    {"PEUGEOT_206", "Peugeot 206 WRC", "Peugeot", 0, "WRC", "4WD"},
    {"PEUGEOT_206.CHAMP", "Peugeot 206 WRC", "Peugeot", 0, "WRC", "4WD"},
    {"PEUGEOT_206_s1600", "Peugeot 206 S1600", "Peugeot", 0, "Super 1600", "FWD"},
    {"207_s2000", "Peugeot 207 S2000", "Peugeot", 0, "S2000", "4WD"},
    {"Peugeot 207 S2000", "Peugeot 207 S2000", "Peugeot", 0, "S2000", "4WD"},
    {"ATR_P307_WRC05", "Peugeot 307 WRC", "Peugeot", 2005, "WRC", "4WD"},
    {"IMPREZA00", "Subaru Impreza 2000", "Subaru", 2000, "", "4WD"},
    {"IMPREZA03", "Subaru Impreza WRC 2003", "Subaru", 2003, "WRC", "4WD"},
    {"IMPREZA03.CHAMP", "Subaru Impreza WRC 2003", "Subaru", 2003, "WRC", "4WD"},
    {"IMPREZA03.N11C", "Subaru Impreza WRC 2003", "Subaru", 2003, "WRC", "4WD"},
    {"IMPREZA05", "Subaru Impreza WRC 2005", "Subaru", 2005, "WRC", "4WD"},
    {"IMPREZA06", "Subaru Impreza WRC 2006", "Subaru", 2006, "WRC", "4WD"},
    {"IMPREZA06_GrN", "Subaru Impreza WRX STI", "Subaru", 2006, "Group N", "4WD"},
    {"COROLLA", "Toyota Corolla WRC", "Toyota", 0, "WRC", "4WD"},
    {"CELICA_st205", "Toyota Celica GT-Four ST205", "Toyota", 0, "Group A", "4WD"},
    {"delta", "Lancia Delta HF Integrale", "Lancia", 0, "Group A", "4WD"},
    {"FORD_FOCUS_2001", "Ford Focus RS WRC 01", "Ford", 2001, "WRC", "4WD"},
    {"Ford_Focus_2003", "Ford Focus RS WRC 03", "Ford", 2003, "WRC", "4WD"},
    {"FOCUS03", "Ford Focus RS WRC 03", "Ford", 2003, "WRC", "4WD"},
    {"FORD_FOCUS_2006", "Ford Focus RS WRC 06", "Ford", 2006, "WRC", "4WD"},
    {"Octavia", "\xC5\xA0koda Octavia WRC", "\xC5\xA0koda", 0, "WRC", "4WD"},
    {"Octavia.CHAMP", "\xC5\xA0koda Octavia WRC", "\xC5\xA0koda", 0, "WRC", "4WD"},
    {"Skoda_Kitcar_A7", "\xC5\xA0koda Kit Car", "\xC5\xA0koda", 0, "Kit car", "FWD"},
    {"CLIO_S1600", "Renault Clio S1600", "Renault", 0, "Super 1600", "FWD"},
    {"PUNTO_1600_ATR2005", "Fiat Punto S1600", "Fiat", 2005, "Super 1600", "FWD"},
    {"i20_R5", "Hyundai i20 R5", "Hyundai", 0, "R5", "4WD"},
    {"yugo", "Zastava Yugo", "Zastava", 0, "", "FWD"},
};

// The classes in the menus' order.
const char* const kClassOrder[] = {"WRC", "S2000", "R5", "Group A", "Group N", "Super 1600", "Kit car"};

} // namespace

void CarRegister::Build(const std::string& rbrPath, const std::vector<Car>& cars)
{
    m_cars.clear();
    for (const Car& car : cars) {
        // 1. RBR's files
        CarInfo info;
        info.id = car.folder;
        info.name = car.name;
        info.from = "RBR";
        // 2. the server
        for (const ServerCar& s : kServerCars)
            if (!_stricmp(s.id, car.folder.c_str())) {
                info.name = s.name, info.maker = s.maker, info.year = s.year, info.carClass = s.carClass, info.drive = s.drive;
                info.known = true, info.from += " + server";
            }
        // 3. car.json
        std::string error;
        const formats::Json j = formats::ReadJsonFile(rbrPath + "\\Cars\\" + car.folder + "\\car.json", error);
        if (error.empty() && j.type == formats::Json::Object) {
            info.name = j.Str("name", info.name);
            info.maker = j.Str("maker", info.maker);
            info.year = static_cast<int>(j.Num("year", info.year));
            info.carClass = j.Str("class", info.carClass);
            info.drive = j.Str("drive", info.drive);
            info.powerHp = static_cast<float>(j.Num("power_hp", info.powerHp));
            info.weightKg = static_cast<float>(j.Num("weight_kg", info.weightKg));
            info.physics = j.Str("physics", info.physics);
            info.known = true, info.from += " + car.json";
        } else if (error != "no file") {
            info.error = "car.json: " + error;
        }
        m_cars.push_back(std::move(info));
    }
}

const CarInfo* CarRegister::Find(const std::string& id) const
{
    for (const CarInfo& c : m_cars)
        if (!_stricmp(c.id.c_str(), id.c_str())) return &c;
    return nullptr;
}

std::vector<std::string> CarRegister::Classes() const
{
    std::vector<std::string> out;
    for (const char* cls : kClassOrder)
        for (const CarInfo& c : m_cars)
            if (c.known && c.carClass == cls) {
                out.push_back(cls);
                break;
            }
    for (const CarInfo& c : m_cars)   // a car.json's own classes after them
        if (c.known && !c.carClass.empty() && std::find(out.begin(), out.end(), c.carClass) == out.end()) out.push_back(c.carClass);
    return out;
}

} // namespace wrally::assets
