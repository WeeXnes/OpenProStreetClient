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
    {"AUDIR8", "Audi R8"},

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
    {"C6", "Chevrolet Corvette C6"},

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

static std::string NormalizeCarKey(const std::string& rawKey) {
    if (rawKey.length() < 2 || rawKey.length() > 64) {
        return "";
    }

    std::string upper = rawKey;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
        return std::toupper(c);
    });

    if (upper.rfind("CAR_", 0) == 0) {
        upper = upper.substr(4);
    }

    // 1. Direct match
    auto it = CAR_TRANSLATION_TABLE.find(upper);
    if (it != CAR_TRANSLATION_TABLE.end()) {
        return it->first;
    }

    // 2. Prefix split before mode suffix (e.g. "CORVETTE_DRAG", "RX7_DRIFT", "GTR_GRIP")
    size_t underPos = upper.find('_');
    if (underPos != std::string::npos) {
        std::string prefix = upper.substr(0, underPos);
        it = CAR_TRANSLATION_TABLE.find(prefix);
        if (it != CAR_TRANSLATION_TABLE.end()) {
            return it->first;
        }
    }

    // 3. Check for matching token
    for (const auto& pair : CAR_TRANSLATION_TABLE) {
        if (upper.length() >= pair.first.length()) {
            if (upper == pair.first || upper.rfind(pair.first + "_", 0) == 0 || upper.find("_" + pair.first) != std::string::npos) {
                return pair.first;
            }
        }
    }

    return "";
}

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
        // Valid car ID characters
        if (isalnum((unsigned char)c) || c == '_' || c == '-') {
            result.push_back(c);
        } else {
            break;
        }
    }

    return result;
}

static std::string CheckDirectOrDerefCarString(uintptr_t ptr) {
    if (!ptr || IsBadReadPtr((void*)ptr, sizeof(uintptr_t))) {
        return "";
    }

    // 1. Direct string check
    std::string s1 = ReadSafeString(ptr);
    std::string norm1 = NormalizeCarKey(s1);
    if (!norm1.empty()) return norm1;

    // 2. Dereferenced +0x00 string check
    uintptr_t deref0 = *(uintptr_t*)ptr;
    if (deref0 && !IsBadReadPtr((void*)deref0, sizeof(uintptr_t))) {
        std::string s2 = ReadSafeString(deref0);
        std::string norm2 = NormalizeCarKey(s2);
        if (!norm2.empty()) return norm2;

        if (!IsBadReadPtr((void*)(deref0 + 0x04), sizeof(uintptr_t))) {
            uintptr_t deref0_4 = *(uintptr_t*)(deref0 + 0x04);
            if (deref0_4 && !IsBadReadPtr((void*)deref0_4, sizeof(uintptr_t))) {
                std::string s2b = ReadSafeString(deref0_4);
                std::string norm2b = NormalizeCarKey(s2b);
                if (!norm2b.empty()) return norm2b;
            }
        }
    }

    // 3. Dereferenced +0x04 string check
    if (!IsBadReadPtr((void*)(ptr + 0x04), sizeof(uintptr_t))) {
        uintptr_t deref4 = *(uintptr_t*)(ptr + 0x04);
        if (deref4 && !IsBadReadPtr((void*)deref4, sizeof(uintptr_t))) {
            std::string s3 = ReadSafeString(deref4);
            std::string norm3 = NormalizeCarKey(s3);
            if (!norm3.empty()) return norm3;

            if (!IsBadReadPtr((void*)(deref4 + 0x04), sizeof(uintptr_t))) {
                uintptr_t deref4_4 = *(uintptr_t*)(deref4 + 0x04);
                if (deref4_4 && !IsBadReadPtr((void*)deref4_4, sizeof(uintptr_t))) {
                    std::string s3b = ReadSafeString(deref4_4);
                    std::string norm3b = NormalizeCarKey(s3b);
                    if (!norm3b.empty()) return norm3b;
                }
            }
        }
    }

    return "";
}

std::string ReadRawCarIdentifier(uintptr_t baseModule) {
    // 1. Check active LocalPlayer singleton (0xBFB400 -> baseModule + 0x7FB400)
    uintptr_t activePlayerBase = baseModule + 0x7FB400;
    if (IsBadReadPtr((void*)activePlayerBase, sizeof(uintptr_t)) == 0) {
        uintptr_t localPlayer = *(uintptr_t*)activePlayerBase;
        if (localPlayer != 0 && IsBadReadPtr((void*)localPlayer, 0x60) == 0) {
            uintptr_t pVehicle = *(uintptr_t*)(localPlayer + 0x28);
            if (pVehicle != 0 && IsBadReadPtr((void*)pVehicle, 0x100) == 0) {
                std::string car = CheckDirectOrDerefCarString(pVehicle + 0x04);
                if (!car.empty()) return car;

                car = CheckDirectOrDerefCarString(*(uintptr_t*)(pVehicle + 0x04));
                if (!car.empty()) return car;

                car = CheckDirectOrDerefCarString(pVehicle);
                if (!car.empty()) return car;

                for (int off = 0x08; off <= 0x80; off += 4) {
                    car = CheckDirectOrDerefCarString(*(uintptr_t*)(pVehicle + off));
                    if (!car.empty()) return car;
                }
            }
        }
    }

    // 2. Check LocalPlayer array (0xACE444 -> baseModule + 0x6CE444)
    uintptr_t localPlayerArrayBase = baseModule + 0x6CE444;
    if (IsBadReadPtr((void*)localPlayerArrayBase, sizeof(uintptr_t)) == 0) {
        uintptr_t arr = *(uintptr_t*)localPlayerArrayBase;
        if (arr != 0 && IsBadReadPtr((void*)arr, sizeof(uintptr_t)) == 0) {
            uintptr_t localPlayer0 = *(uintptr_t*)arr;
            if (localPlayer0 != 0 && IsBadReadPtr((void*)localPlayer0, 0x60) == 0) {
                uintptr_t pVehicle = *(uintptr_t*)(localPlayer0 + 0x28);
                if (pVehicle != 0 && IsBadReadPtr((void*)pVehicle, 0x100) == 0) {
                    std::string car = CheckDirectOrDerefCarString(pVehicle + 0x04);
                    if (!car.empty()) return car;

                    car = CheckDirectOrDerefCarString(*(uintptr_t*)(pVehicle + 0x04));
                    if (!car.empty()) return car;

                    car = CheckDirectOrDerefCarString(pVehicle);
                    if (!car.empty()) return car;
                }

                static const int playerOffsets[] = { 0x14, 0x0C, 0x10, 0x18, 0x20, 0x24, 0x30, 0x34, 0x48, 0x5C, 0x88 };
                for (int off : playerOffsets) {
                    uintptr_t candidate = *(uintptr_t*)(localPlayer0 + off);
                    std::string car = CheckDirectOrDerefCarString(candidate);
                    if (!car.empty()) return car;
                }
            }
        }
    }

    // 3. Check GRaceStatus singleton (0xAB9D7C -> baseModule + 0x6B9D7C)
    uintptr_t gRaceStatusBase = baseModule + 0x6B9D7C;
    if (IsBadReadPtr((void*)gRaceStatusBase, sizeof(uintptr_t)) == 0) {
        uintptr_t gRaceStatus = *(uintptr_t*)gRaceStatusBase;
        if (gRaceStatus != 0 && IsBadReadPtr((void*)(gRaceStatus + 0x20), 0x20) == 0) {
            // First pass: local player slot (flags & 0x02 at racer + 0x68)
            for (int i = 0; i < 8; ++i) {
                uintptr_t racer = gRaceStatus + 0x20 + (i * 0x594);
                if (IsBadReadPtr((void*)racer, 0x80)) {
                    break;
                }

                uint32_t flags = *(uint32_t*)(racer + 0x68);
                if (flags & 0x02) { // Local human player
                    static const int racerOffsets[] = { 0x0C, 0x10, 0x08, 0x04, 0x14, 0x18, 0x20, 0x24, 0x28 };
                    for (int off : racerOffsets) {
                        uintptr_t candidate = *(uintptr_t*)(racer + off);
                        std::string car = CheckDirectOrDerefCarString(candidate);
                        if (!car.empty()) return car;
                    }
                }
            }

            // Second pass: check racer 0
            uintptr_t racer0 = gRaceStatus + 0x20;
            if (IsBadReadPtr((void*)racer0, 0x80) == 0) {
                static const int racerOffsets[] = { 0x0C, 0x10, 0x08, 0x04, 0x14, 0x18 };
                for (int off : racerOffsets) {
                    uintptr_t candidate = *(uintptr_t*)(racer0 + off);
                    std::string car = CheckDirectOrDerefCarString(candidate);
                    if (!car.empty()) return car;
                }
            }
        }
    }

    return "";
}

std::string TranslateCarIdentifier(const std::string& rawIdentifier) {
    std::string norm = NormalizeCarKey(rawIdentifier);
    if (norm.empty()) {
        norm = rawIdentifier;
    }

    auto it = CAR_TRANSLATION_TABLE.find(norm);
    if (it != CAR_TRANSLATION_TABLE.end()) {
        return it->second;
    }

    return rawIdentifier;
}
