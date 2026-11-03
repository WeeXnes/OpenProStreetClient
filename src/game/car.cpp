#include "game/car.h"
#include "game/race_mode.h"
#include "log.h"
#include <unordered_map>
#include <cstdio>

// Attrib::StringHash32 of each "pvehicle" collection name -> CarTypeInfo model name.
// This is the value stored in FECarRecord +0x08 (returned by DALVehicleCommands::GetPVehicleKey).
static const std::unordered_map<uint32_t, std::string> VEHICLE_KEY_TO_MODEL = {
    {0xA86F2BFC, "240SX"}, // 240sx
    {0x35138539, "350Z"}, // 350z
    {0x43CB4B89, "997GT3RS"}, // 997
    {0xD494B7B0, "997GT2"}, // 997gt2
    {0x5A7F9439, "997TT"}, // 997tt
    {0xEE55498A, "997TT"}, // 997tt_slotcar
    {0xDDA4F129, "BMWM3"}, // bmwm3
    {0x29C56468, "BMWM3E92"}, // bmwm3e92
    {0x481A9500, "BMWMZ4"}, // bmwmz4
    {0x90944519, "CAMARO"}, // camaro
    {0xC097A400, "CAMARON"}, // camaron
    {0x41555BEB, "CAYMANS"}, // caymans
    {0xA3906772, "CHALLENGER71"}, // challenger71
    {0x868D6F75, "CHARGER69"}, // charger69
    {0x03E07E4E, "CHEVELLE"}, // chevelle
    {0xF5F12571, "CIVICHB"}, // civichb
    {0x3C8E22A2, "CIVICSI"}, // civicsi
    {0x5A784095, "COBALTSS"}, // cobaltss
    {0x35124316, "COROLLA"}, // corolla
    {0x46118CE4, "CORVETTE"}, // corvette
    {0xB5804C5E, "CORVETTE67"}, // corvette67
    {0x6E7A4133, "CORVETTEZ06"}, // corvettez06
    {0xE14EA776, "COSWORTH"}, // cosworth
    {0x9E0638BF, "CTSV"}, // ctsv
    {0x5F6DF4C6, "CUDA"}, // cuda
    {0x61458871, "997GT3"}, // dlcbp_997gt3
    {0x6ABEBCD3, "997GT3RS"}, // dlcbp_997gt3rs
    {0x5F613893, "CARRERAGT"}, // dlcbp_carreragt
    {0xB549BBCC, "CCX"}, // dlcbp_ccx
    {0xF57E7854, "CHALLENGERN"}, // dlcbp_challengern
    {0x647F5151, "DB9"}, // dlcbp_db9
    {0x44634919, "DBR9"}, // dlcbp_dbr9
    {0x4D105407, "DELTA"}, // dlcbp_delta
    {0x0FCD39E3, "GALLARDOS"}, // dlcbp_gallardos
    {0xE5A2AB11, "LEONCUPRA"}, // dlcbp_leoncupra
    {0x9CFB5951, "MCLARENF1"}, // dlcbp_mclarenf1
    {0xBCEE5129, "R8PROD"}, // dlcbp_r8prod
    {0xFBE10B4D, "ROADRUNNER"}, // dlcbp_roadrunner
    {0x69B39015, "S2000"}, // dlcbp_s2000
    {0x25F4992E, "SL65"}, // dlcbp_sl65
    {0x3D634591, "VEYRON"}, // dlcbp_veyron
    {0xB3BF8516, "ECLIPSE"}, // eclipse
    {0x313D4936, "ELISE"}, // elise
    {0x1FB6E0E6, "FOCUSST"}, // focusst
    {0x42C0A5EA, "FORDGT"}, // fordgt
    {0xE73E8E70, "G35"}, // g35
    {0x79054B53, "GTI"}, // gti
    {0xAA483DC6, "GTO"}, // gto
    {0xE6E22BE4, "GTO65"}, // gto65
    {0xA5558E31, "GTRPROTO"}, // gtrproto
    {0xEE484B2B, "SKYLINER35"}, // gtrproto2
    {0xA232EDC5, "IMPREZAWRXSTI"}, // imprezawrxsti
    {0x1193D233, "INTEGRALS"}, // integrals
    {0x85106818, "INTEGRATYPER"}, // integratyper
    {0xEC764047, "IS350"}, // is350
    {0x7D41944D, "LANCEREVO9"}, // lancerevo9
    {0x6B8F9A20, "LANCEREVOX"}, // lancerevox
    {0x7076AC12, "MAZDASPEED3"}, // mazdaspeed3
    {0xE4DB67B3, "MURCIELAGO640"}, // murcielago640
    {0xA9DE680B, "MUSTANG03"}, // mustang03
    {0x9B93FD4D, "MUSTANGGT"}, // mustanggt
    {0xB927B74E, "MUSTANGSHLBYN"}, // mustangshlbyn
    {0x02A60A50, "MUSTANGSHLBYO"}, // mustangshlbyo
    {0x033115AF, "NSX"}, // nsx
    {0x942BBF21, "R32"}, // r32
    {0x4771A048, "RS4"}, // rs4
    {0x00E489BB, "RSX"}, // rsx
    {0x8E09C833, "RX7"}, // rx7
    {0xD6334437, "RX8"}, // rx8
    {0xF44314F8, "S3"}, // s3
    {0x80D5F2D9, "S4"}, // s4
    {0xF0A9260B, "LANCEREVOX"}, // showdown_king
    {0xE7ABDF48, "SILVIA"}, // silvia
    {0xDBA80199, "SKYLINE"}, // skyline
    {0xFB5F2DE0, "SKYLINER35"}, // skyliner35
    {0x373DFD47, "SOLSTICEGXP"}, // solsticegxp
    {0x66F13434, "SUPRA"}, // supra
    {0x3AF6861B, "TTN"}, // ttn
    {0x6EBED408, "VIPER"}, // viper
    {0xAD54F689, "ZONDA"} // zonda
};

// Display names follow the game's own CARNAME_* strings (LANGUAGES\English_Global.bin).
static const std::unordered_map<std::string, std::string> CAR_TRANSLATION_TABLE = {
    // Acura
    {"INTEGRALS", "Acura Integra"},
    {"INTEGRATYPER", "Acura Integra Type R"},
    {"NSX", "Acura NSX"},
    {"RSX", "Acura RSX"},

    // Aston Martin
    {"DB9", "Aston Martin DB9"},
    {"DBR9", "Aston Martin DBR9"},

    // Audi
    {"R8PROD", "Audi R8"},
    {"RS4", "Audi RS4"},
    {"S3", "Audi S3"},
    {"S4", "Audi S4"},
    {"TTN", "Audi TT 3.2 quattro"},

    // BMW
    {"BMWM3", "BMW M3 E46"},
    {"BMWM3E92", "BMW M3 E92"},
    {"BMWMZ4", "BMW Z4 M Coupe"},

    // Bugatti
    {"VEYRON", "Bugatti Veyron 16.4"},

    // Cadillac
    {"CTSV", "Cadillac CTS-V"},

    // Chevrolet
    {"CAMARO", "Chevrolet Camaro SS"},
    {"CAMARON", "Chevrolet Camaro Concept"},
    {"CHEVELLE", "Chevrolet Chevelle SS"},
    {"COBALTSS", "Chevrolet Cobalt SS"},
    {"CORVETTE", "Chevrolet Corvette C6"},
    {"CORVETTE67", "Chevrolet Corvette '67"},
    {"CORVETTEZ06", "Chevrolet Corvette Z06"},

    // Dodge
    {"CHALLENGER71", "Dodge Challenger R/T"},
    {"CHALLENGERN", "Dodge Challenger Concept"},
    {"CHARGER69", "Dodge Charger R/T"},
    {"VIPER", "Dodge Viper SRT10"},

    // Ford
    {"COSWORTH", "Ford Escort RS Cosworth"},
    {"FOCUSST", "Ford Focus ST"},
    {"FORDGT", "Ford GT"},
    {"MUSTANG03", "Ford Mustang GT '03"},
    {"MUSTANGGT", "Ford Mustang GT"},

    // Honda
    {"CIVICHB", "Honda Civic"},
    {"CIVICSI", "Honda Civic Si"},
    {"S2000", "Honda S2000"},

    // Infiniti
    {"G35", "Infiniti G35 (V35)"},

    // Koenigsegg
    {"CCX", "Koenigsegg CCX"},

    // Lamborghini
    {"GALLARDOS", "Lamborghini Gallardo Superleggera"},
    {"MURCIELAGO640", "Lamborghini Murciélago LP640"},

    // Lancia
    {"DELTA", "Lancia Delta Integrale Evo"},

    // Lexus
    {"IS350", "Lexus IS350"},

    // Lotus
    {"ELISE", "Lotus Elise"},

    // Mazda
    {"MAZDASPEED3", "Mazda Mazdaspeed3"},
    {"RX7", "Mazda RX-7"},
    {"RX8", "Mazda RX-8"},

    // McLaren
    {"MCLARENF1", "McLaren F1"},

    // Mercedes-Benz
    {"SL65", "Mercedes-Benz SL65 AMG"},

    // Mitsubishi
    {"ECLIPSE", "Mitsubishi Eclipse"},
    {"LANCEREVO9", "Mitsubishi Lancer Evolution IX"},
    {"LANCEREVOX", "Mitsubishi Lancer Evolution X"},

    // Nissan
    {"240SX", "Nissan 240SX (S13)"},
    {"350Z", "Nissan 350Z (Z33)"},
    {"GTRPROTO", "Nissan GT-R Proto"},
    {"SILVIA", "Nissan Silvia (S15)"},
    {"SKYLINE", "Nissan Skyline GT-R (R34)"},
    {"SKYLINER35", "Nissan GT-R (R35)"},

    // Pagani
    {"ZONDA", "Pagani Zonda F"},

    // Plymouth
    {"CUDA", "Plymouth Hemi Cuda"},
    {"ROADRUNNER", "Plymouth Road Runner"},

    // Pontiac
    {"GTO", "Pontiac GTO"},
    {"GTO65", "Pontiac GTO '65"},
    {"SOLSTICEGXP", "Pontiac Solstice GXP"},

    // Porsche
    {"997GT2", "Porsche 911 GT2"},
    {"997GT3", "Porsche 911 GT3"},
    {"997GT3RS", "Porsche 911 GT3 RS"},
    {"997TT", "Porsche 911 Turbo"},
    {"CARRERAGT", "Porsche Carrera GT"},
    {"CAYMANS", "Porsche Cayman S"},

    // SEAT
    {"LEONCUPRA", "SEAT Leon Cupra"},

    // Shelby
    {"MUSTANGSHLBYN", "Shelby GT500"},
    {"MUSTANGSHLBYO", "Shelby GT500 '67"},

    // Subaru
    {"IMPREZAWRXSTI", "Subaru Impreza WRX STi"},

    // Toyota
    {"COROLLA", "Toyota Corolla GTS (AE86)"},
    {"SUPRA", "Toyota Supra"},

    // Volkswagen
    {"GTI", "Volkswagen Golf GTI"},
    {"R32", "Volkswagen R32"}
};

static const uint32_t INVALID_CAR_HANDLE = 0xFFFFFFFF;
static const int FE_CAR_RECORD_COUNT = 0x19A;
static const int FE_CAR_RECORD_SIZE = 0x18;

bool IsCurrentCarConfirmed(uintptr_t baseModule) {
    static uint32_t savedHandle = INVALID_CAR_HANDLE;
    static bool confirmed = false;

    uintptr_t careerBase = baseModule + 0x6B0EA8;
    if (IsBadReadPtr((void*)careerBase, sizeof(uintptr_t))) {
        return false;
    }
    uintptr_t career = *(uintptr_t*)careerBase;
    if (career == 0 || IsBadReadPtr((void*)career, 0x1D8)) {
        return false;
    }

    uint32_t carHandle = *(uint32_t*)(career + 0x18);
    if (carHandle == INVALID_CAR_HANDLE) {
        savedHandle = INVALID_CAR_HANDLE;
        confirmed = false;
        return false;
    }

    // The slot starts out as whatever the save file stored, which can be a car from an earlier
    // session. It is only known to be current once a race uses it, the game changes it, or the
    // game's career car table (+0x20, filled when an event starts) lists it.
    if (savedHandle == INVALID_CAR_HANDLE) {
        savedHandle = carHandle;
    }
    if (confirmed || carHandle != savedHandle || IsRaceActive(baseModule)) {
        confirmed = true;
        return true;
    }

    for (uintptr_t off = 0x20; off < 0x1D8; off += sizeof(uint32_t)) {
        if (*(uint32_t*)(career + off) == carHandle) {
            confirmed = true;
            return true;
        }
    }

    return false;
}

std::string ReadRawCarIdentifier(uintptr_t baseModule) {
    // FE::Career::FECareer (0xAB0EA8) +0x18 holds the local player's car handle (DALCareer id 0x1772).
    uintptr_t careerBase = baseModule + 0x6B0EA8;
    if (IsBadReadPtr((void*)careerBase, sizeof(uintptr_t))) {
        return "";
    }
    uintptr_t career = *(uintptr_t*)careerBase;
    if (career == 0 || IsBadReadPtr((void*)(career + 0x18), sizeof(uint32_t))) {
        return "";
    }
    uint32_t carHandle = *(uint32_t*)(career + 0x18);
    if (carHandle == INVALID_CAR_HANDLE) {
        return "";
    }
    return CarModelForHandle(baseModule, carHandle);
}

std::string CarModelForHandle(uintptr_t baseModule, uint32_t carHandle) {
    // UserProfile (0xAB0EAC) +0x240 is the FEPlayerCarDB record table.
    uintptr_t profileBase = baseModule + 0x6B0EAC;
    if (IsBadReadPtr((void*)profileBase, sizeof(uintptr_t))) {
        return "";
    }
    uintptr_t profile = *(uintptr_t*)profileBase;
    if (profile == 0) {
        return "";
    }
    uintptr_t records = profile + 0x240;
    if (IsBadReadPtr((void*)records, FE_CAR_RECORD_COUNT * FE_CAR_RECORD_SIZE)) {
        return "";
    }

    for (int i = 0; i < FE_CAR_RECORD_COUNT; ++i) {
        uintptr_t record = records + (i * FE_CAR_RECORD_SIZE);
        if (*(uint32_t*)record != carHandle) {
            continue;
        }

        uint32_t vehicleKey = *(uint32_t*)(record + 0x08);
        auto it = VEHICLE_KEY_TO_MODEL.find(vehicleKey);
        if (it != VEHICLE_KEY_TO_MODEL.end()) {
            return it->second;
        }

        static uint32_t lastUnknownKey = 0;
        if (vehicleKey != lastUnknownKey) {
            Log::Debug("[OpenProStreet] COULDNT FIND CAR FOR VEHICLE KEY: 0x%08X\n", vehicleKey);
            lastUnknownKey = vehicleKey;
        }
        return "";
    }

    return "";
}

std::string TranslateCarIdentifier(const std::string& rawIdentifier) {
    auto it = CAR_TRANSLATION_TABLE.find(rawIdentifier);
    if (it != CAR_TRANSLATION_TABLE.end()) {
        return it->second;
    }

    return rawIdentifier;
}
