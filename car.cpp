#include "car.h"
#include <unordered_map>
#include <algorithm>
#include <cctype>

static const std::unordered_map<std::string, std::string> CAR_TRANSLATION_TABLE = {
    // Acura
    {"INTEGRA", "Acura Integra Type R"},
    {"RSX", "Acura RSX Type-S"},
    {"NSX", "Acura NSX"},

    // Audi
    {"S3", "Audi S3"},
    {"S4", "Audi S4"},
    {"RS4", "Audi RS4"},
    {"TT", "Audi TT 3.2 quattro"},
    {"R8", "Audi R8"},
    {"AUDI_R8", "Audi R8"},

    // BMW
    {"E46", "BMW M3 (E46)"},
    {"M3E46", "BMW M3 (E46)"},
    {"E92", "BMW M3 (E92)"},
    {"M3E92", "BMW M3 (E92)"},
    {"M3", "BMW M3"},
    {"Z4", "BMW Z4 M Coupe"},
    {"Z4M", "BMW Z4 M Coupe"},

    // Cadillac
    {"CTS_V", "Cadillac CTS-V"},
    {"CTSV", "Cadillac CTS-V"},

    // Chevrolet
    {"CAMARO", "Chevrolet Camaro SS"},
    {"CAMAROSS", "Chevrolet Camaro SS"},
    {"CAMAROCONCEPT", "Chevrolet Camaro Concept"},
    {"CHEVELLE", "Chevrolet Chevelle SS"},
    {"COBALT", "Chevrolet Cobalt SS"},
    {"COBALTSS", "Chevrolet Cobalt SS"},
    {"CORVETTE", "Chevrolet Corvette Z06"},
    {"CORVETTEZ06", "Chevrolet Corvette Z06"},
    {"CORVETTESTINGRAY", "Chevrolet Corvette Stingray (1967)"},
    {"CORVETTE67", "Chevrolet Corvette (1967)"},
    {"CORVETTE_67", "Chevrolet Corvette (1967)"},
    {"CORVETTE_C6", "Chevrolet Corvette C6"},

    // Dodge
    {"CHARGER", "Dodge Charger R/T"},
    {"CHARGER69", "Dodge Charger R/T (1969)"},
    {"CHALLENGER", "Dodge Challenger R/T"},
    {"CHALLENGER71", "Dodge Challenger (1971)"},
    {"VIPER", "Dodge Viper SRT10"},
    {"VIPERSRT10", "Dodge Viper SRT10"},
    {"VIPER_COUPE", "Dodge Viper SRT10 Coupe"},

    // Ford
    {"ESCORT", "Ford Escort RS Cosworth"},
    {"ESCORTCOSWORTH", "Ford Escort RS Cosworth"},
    {"FOCUS", "Ford Focus ST"},
    {"FOCUSST", "Ford Focus ST"},
    {"GT", "Ford GT"},
    {"FORDGT", "Ford GT"},
    {"MUSTANG", "Ford Mustang GT"},
    {"MUSTANGGT", "Ford Mustang GT"},
    {"MUSTANG67", "Ford Mustang (1967)"},
    {"MUSTANG_SHELBY", "Shelby GT500"},

    // Honda
    {"CIVIC", "Honda Civic Si"},
    {"CIVICSI", "Honda Civic Si"},
    {"S2000", "Honda S2000"},

    // Infiniti
    {"G35", "Infiniti G35"},

    // Lamborghini & Koenigsegg & Pagani
    {"MURCIELAGO", "Lamborghini Murciélago LP640"},
    {"MURCIELAGOLP640", "Lamborghini Murciélago LP640"},
    {"LP640", "Lamborghini Murciélago LP640"},
    {"GALLARDO", "Lamborghini Gallardo Superleggera"},
    {"GALLARDO_SUPERLEGGERA", "Lamborghini Gallardo Superleggera"},
    {"CCX", "Koenigsegg CCX"},
    {"KOENIGSEGG_CCX", "Koenigsegg CCX"},
    {"PAGANI", "Pagani Zonda F"},
    {"ZONDA", "Pagani Zonda F"},
    {"ZONDAF", "Pagani Zonda F"},

    // Lexus
    {"IS350", "Lexus IS350"},

    // Lotus
    {"ELISE", "Lotus Elise 111R"},

    // Mazda
    {"MAZDA3", "Mazdaspeed 3"},
    {"MAZDASPEED3", "Mazdaspeed 3"},
    {"RX7", "Mazda RX-7"},
    {"RX8", "Mazda RX-8"},
    {"MIATA", "Mazda MX-5"},
    {"MX5", "Mazda MX-5"},

    // Mercedes-Benz & McLaren
    {"SL65", "Mercedes-Benz SL 65 AMG"},
    {"SL65AMG", "Mercedes-Benz SL 65 AMG"},
    {"SLR", "Mercedes-Benz SLR McLaren 722"},
    {"SLRMCLAREN", "Mercedes-Benz SLR McLaren 722"},

    // Mitsubishi
    {"ECLIPSE", "Mitsubishi Eclipse GSX"},
    {"EVO9", "Mitsubishi Lancer Evolution IX MR"},
    {"LANCEREVO9", "Mitsubishi Lancer Evolution IX MR"},
    {"EVOX", "Mitsubishi Lancer Evolution X"},
    {"LANCEREVOX", "Mitsubishi Lancer Evolution X"},

    // Nissan
    {"240SX", "Nissan 240SX (S13)"},
    {"SILVIA", "Nissan Silvia (S15)"},
    {"S15", "Nissan Silvia (S15)"},
    {"350Z", "Nissan 350Z (Z33)"},
    {"Z33", "Nissan 350Z (Z33)"},
    {"SKYLINE", "Nissan Skyline GT-R (R34)"},
    {"R34", "Nissan Skyline GT-R (R34)"},
    {"GTR", "Nissan GT-R (R35)"},
    {"GTR35", "Nissan GT-R (R35)"},
    {"GTR_PROTO", "Nissan GT-R Proto"},

    // Plymouth
    {"HEMICUDA", "Plymouth Hemi Cuda"},
    {"CUDA", "Plymouth Hemi Cuda"},
    {"ROADRUNNER", "Plymouth Road Runner"},

    // Pontiac
    {"GTO", "Pontiac GTO"},
    {"GTO65", "Pontiac GTO (1965)"},
    {"GTO05", "Pontiac GTO (2005)"},
    {"SOLSTICE", "Pontiac Solstice GXP"},
    {"SOLSTICEST", "Pontiac Solstice GXP"},
    {"FIREBIRD", "Pontiac Firebird"},

    // Porsche
    {"911GT2", "Porsche 911 GT2"},
    {"997GT2", "Porsche 911 GT2 (997)"},
    {"911TURBO", "Porsche 911 Turbo"},
    {"997TURBO", "Porsche 911 Turbo (997)"},
    {"911GT3", "Porsche 911 GT3 RS"},
    {"997GT3RS", "Porsche 911 GT3 RS (997)"},
    {"GT3RS", "Porsche 911 GT3 RS"},
    {"CAYMAN", "Porsche Cayman S"},
    {"CAYMANS", "Porsche Cayman S"},
    {"CARRERAGT", "Porsche Carrera GT"},

    // Shelby
    {"GT500", "Shelby GT500"},
    {"GT500KR", "Shelby GT500KR"},
    {"COBRA", "Shelby Cobra 427"},
    {"COBRA427", "Shelby Cobra 427"},
    {"DAYTONA", "Shelby Daytona Coupe"},

    // Subaru
    {"IMPREZA", "Subaru Impreza WRX STi"},
    {"WRX", "Subaru Impreza WRX STi"},
    {"WRXSTI", "Subaru Impreza WRX STi"},

    // Toyota
    {"COROLLA", "Toyota Corolla GTS (AE86)"},
    {"AE86", "Toyota Corolla GTS (AE86)"},
    {"MR2", "Toyota MR2"},
    {"SUPRA", "Toyota Supra"},

    // Volkswagen
    {"GOLF", "Volkswagen Golf GTI"},
    {"GTI", "Volkswagen Golf GTI"},
    {"GOLFGTI", "Volkswagen Golf GTI"},
    {"R32", "Volkswagen Golf R32"}
};

static std::string ReadSafeString(uintptr_t strPtr, size_t maxLen = 32) {
    if (!strPtr || IsBadReadPtr((void*)strPtr, 1)) {
        return "";
    }

    std::string result;
    result.reserve(16);
    for (size_t i = 0; i < maxLen; ++i) {
        if (IsBadReadPtr((void*)(strPtr + i), 1)) {
            break;
        }
        char c = *(char*)(strPtr + i);
        if (c == '\0') {
            break;
        }
        // Valid car ID characters (alphanumeric, underscore, hyphen)
        if (isalnum((unsigned char)c) || c == '_' || c == '-') {
            result.push_back(c);
        } else {
            break;
        }
    }

    if (result.length() < 2) {
        return "";
    }

    return result;
}

static std::string TryExtractCarString(uintptr_t ptr) {
    if (!ptr || IsBadReadPtr((void*)ptr, sizeof(uintptr_t))) {
        return "";
    }

    // 1. Try reading ptr as a direct C-string pointer
    std::string str = ReadSafeString(ptr);
    if (!str.empty() && str.length() >= 2) {
        return str;
    }

    // 2. Try reading *(ptr + 0x4) as a C-string pointer
    if (!IsBadReadPtr((void*)(ptr + 0x4), sizeof(uintptr_t))) {
        uintptr_t subPtr = *(uintptr_t*)(ptr + 0x4);
        str = ReadSafeString(subPtr);
        if (!str.empty() && str.length() >= 2) {
            return str;
        }
    }

    // 3. Try reading *(ptr + 0x0) as a C-string pointer
    uintptr_t derefPtr = *(uintptr_t*)ptr;
    str = ReadSafeString(derefPtr);
    if (!str.empty() && str.length() >= 2) {
        return str;
    }

    return "";
}

std::string ReadRawCarIdentifier(uintptr_t baseModule) {
    // 1. Check GRaceStatus singleton (0xAB9D7C) -> scan racers (0..7)
    uintptr_t gRaceStatusBase = baseModule + 0x6B9D7C;
    if (IsBadReadPtr((void*)gRaceStatusBase, sizeof(uintptr_t)) == 0) {
        uintptr_t gRaceStatus = *(uintptr_t*)gRaceStatusBase;
        if (gRaceStatus != 0 && IsBadReadPtr((void*)(gRaceStatus + 0x20), 0x20) == 0) {
            // First pass: look for local player racer (flags & 0x02 at racer + 0x68)
            for (int i = 0; i < 8; ++i) {
                uintptr_t racer = gRaceStatus + 0x20 + (i * 0x594);
                if (IsBadReadPtr((void*)racer, 0x70)) {
                    break;
                }

                uint32_t flags = *(uint32_t*)(racer + 0x68);
                if (flags & 0x02) { // Local human player
                    // Try offset 0x0C (car pointer / handle)
                    uintptr_t ptr0c = *(uintptr_t*)(racer + 0x0C);
                    std::string car = TryExtractCarString(ptr0c);
                    if (!car.empty()) return car;

                    // Try offset 0x10 (player pointer)
                    uintptr_t ptr10 = *(uintptr_t*)(racer + 0x10);
                    car = TryExtractCarString(ptr10);
                    if (!car.empty()) return car;

                    // Try offset 0x08, 0x14
                    car = TryExtractCarString(*(uintptr_t*)(racer + 0x08));
                    if (!car.empty()) return car;
                    car = TryExtractCarString(*(uintptr_t*)(racer + 0x14));
                    if (!car.empty()) return car;
                }
            }

            // Second pass: check racer 0, 1, 2...
            for (int i = 0; i < 8; ++i) {
                uintptr_t racer = gRaceStatus + 0x20 + (i * 0x594);
                if (IsBadReadPtr((void*)racer, 0x70)) {
                    break;
                }

                uintptr_t ptr0c = *(uintptr_t*)(racer + 0x0C);
                std::string car = TryExtractCarString(ptr0c);
                if (!car.empty()) return car;

                uintptr_t ptr10 = *(uintptr_t*)(racer + 0x10);
                car = TryExtractCarString(ptr10);
                if (!car.empty()) return car;
            }
        }
    }

    // 2. Check LocalPlayer array (0xACE444)
    uintptr_t localPlayerCountBase = baseModule + 0x6CE44C;
    uintptr_t localPlayerArrayBase = baseModule + 0x6CE444;
    if (IsBadReadPtr((void*)localPlayerCountBase, sizeof(uintptr_t)) == 0 &&
        IsBadReadPtr((void*)localPlayerArrayBase, sizeof(uintptr_t)) == 0) {
        uintptr_t count = *(uintptr_t*)localPlayerCountBase;
        uintptr_t arr = *(uintptr_t*)localPlayerArrayBase;
        if (count > 0 && arr != 0 && IsBadReadPtr((void*)arr, sizeof(uintptr_t)) == 0) {
            uintptr_t localPlayer0 = *(uintptr_t*)arr;
            if (localPlayer0 != 0 && IsBadReadPtr((void*)localPlayer0, 0x60) == 0) {
                std::string car = TryExtractCarString(*(uintptr_t*)(localPlayer0 + 0x28));
                if (!car.empty()) return car;

                car = TryExtractCarString(*(uintptr_t*)(localPlayer0 + 0x14));
                if (!car.empty()) return car;

                car = TryExtractCarString(*(uintptr_t*)(localPlayer0 + 0x0C));
                if (!car.empty()) return car;
            }
        }
    }

    return "";
}

std::string TranslateCarIdentifier(const std::string& rawIdentifier) {
    if (rawIdentifier.empty()) {
        return "";
    }

    std::string upperKey = rawIdentifier;
    std::transform(upperKey.begin(), upperKey.end(), upperKey.begin(), [](unsigned char c) {
        return std::toupper(c);
    });

    auto it = CAR_TRANSLATION_TABLE.find(upperKey);
    if (it != CAR_TRANSLATION_TABLE.end()) {
        return it->second;
    }

    // Check without prefix/suffix if any
    for (const auto& pair : CAR_TRANSLATION_TABLE) {
        if (upperKey.find(pair.first) != std::string::npos) {
            return pair.second;
        }
    }

    return rawIdentifier;
}
