#include "pch.h"
#include <TlHelp32.h>
#include <Psapi.h>
#include <vector>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <unordered_map>
#include <iostream>
#include <iomanip>
#include <map>
#include <thread>
#include <chrono>

#define WIN32_LEAN_AND_MEAN

using u8 = uint8_t;
using u64 = uint64_t;

// ======= DUMPER OPTIONS CONFIG =======
struct DumperOptions {
    // Main dump modes
    bool enableTotalDump = true;             // Complete classes/fields/methods dump
    bool enableImportantDump = true;         // Only target classes and position/damage data
    bool enableVectorTracking = false;       // Live vector position tracking
    bool enableStringRelation = true;        // Track strings related to methods/fields/classes
    
    // Filtering options
    bool enableRealTimeOutput = true;        // Write files as data is found
    bool enableComprehensiveDump = true;     // Include ALL classes (not just targets)
    bool enableAddressDiscovery = true;      // Find current addresses dynamically
    bool enableConnectionAnalysis = true;    // Build class relationship graph
    
    // Output options
    bool enableConsoleMonitoring = true;     // Live console display
    bool enableDetailedLogging = true;       // Verbose logging
    bool enableFileGrouping = true;          // Group outputs by type (classes, methods, etc.)
    
    // Vector tracking specific
    bool enablePositionVectorTracking = false;  // Track X,Y,Z position changes
    bool enableDamageVectorTracking = false;    // Track health/damage changes
    bool enableVectorChangeLogging = false;     // Log every vector change
    
    // String filtering
    bool enableAllStringDump = false;        // Dump ALL strings (can be huge)
    bool enableRelatedStringsOnly = true;    // Only strings related to classes/methods
    bool enableImportantStringsOnly = true;  // Only game-related strings
};

// Global options instance
static DumperOptions g_options;

static int kNameOff = 0x30;      
static int kNsOff = 0x38;        
static int kParentOff = 0x58;    
static int kFieldsOff = 0x80;    
static int kMethodsOff = 0x98;   
static int kFieldCountOff = 0x9C; 
static int kMethodCountOff = 0xFC; 
// ======================

static std::mutex g_logMtx;
static std::FILE* g_log = nullptr;
static std::atomic<int> g_classCount{ 0 };
static std::atomic<int> g_connectionCount{ 0 };
static std::atomic<int> g_targetClassCount{ 0 };
static std::atomic<int> g_stringCount{ 0 };
static std::atomic<int> g_addressCount{ 0 };
static std::atomic<int> g_vectorChangeCount{ 0 };

// Enhanced data structures for comprehensive dumping
struct Vector3 {
    float x, y, z;
    Vector3() : x(0), y(0), z(0) {}
    Vector3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    
    bool operator!=(const Vector3& other) const {
        return abs(x - other.x) > 0.001f || abs(y - other.y) > 0.001f || abs(z - other.z) > 0.001f;
    }
    
    std::string toString() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << "(" << x << ", " << y << ", " << z << ")";
        return oss.str();
    }
};

struct TrackedVector {
    void* address;
    Vector3 lastValue;
    Vector3 currentValue;
    std::string name;
    std::string className;
    std::string context;
    int changeCount;
    std::chrono::steady_clock::time_point lastUpdate;
    std::vector<std::string> changeHistory;
    bool isPositionVector;
    bool isDamageVector;
};

// ñàìàÿ êîí÷åííàÿ õóéíÿ. Çàôèêñèòü íàäî
struct FoundAddress {
    std::string name;
    std::string className;
    std::string type; // "Method", "Class", "Field"
    void* address;
    std::string signature;
    bool isImportant = false;
    std::vector<std::string> relatedStrings; // Strings related to this address
};

struct ClassInfo {
    std::string name;
    std::string nameSpace;
    std::string fullName;
    void* address;
    std::vector<std::string> fields;
    std::vector<std::string> methods;
    std::vector<FoundAddress> foundAddresses;
    std::vector<TrackedVector> trackedVectors;
    std::vector<std::string> parentChain;
    std::vector<std::string> connections;
    std::vector<std::string> relatedStrings;
    bool isTargetClass = false;
    bool hasPositionData = false;
    bool hasDamageData = false;
};

struct ConnectionInfo {
    std::string fromClass;
    std::string toClass;
    std::string connectionType;
    std::string details;
    std::vector<std::string> relatedStrings;
};

// Collections
static std::vector<ClassInfo> g_allClasses;           
static std::vector<std::string> g_allStrings;         
static std::vector<ConnectionInfo> g_allConnections;  
static std::vector<FoundAddress> g_allFoundAddresses; 
static std::vector<TrackedVector> g_allTrackedVectors;
static std::unordered_map<std::string, ClassInfo> g_targetClasses;
static std::unordered_map<std::string, std::vector<std::string>> g_stringToClassMap; // String -> Classes that reference it
static std::mutex g_dataMutex;

// Console settings
static bool g_liveMonitoring = false;
static HANDLE g_consoleHandle = nullptr;

// Target classes to find (ðàáîòàåò ÷åðåç ðàç. ÔÈÊÑ)
static std::vector<std::string> g_targetClassNames = {
    "GameManager", "EntityManager", "EventManager", "InteractionManager",
    "ItemModule", "LoadingManager", "UIManager", "PlayerModule", 
    "MapManager", "NetworkManager", "ScenePropManager", "BaseEntity",
    "AvatarPropDictionary", "LCChestPlugin", "SceneTreeObject",
    "Singleton_1_EntityManager_", "Singleton_1_EventManager_", 
    "Singleton_1_InteractionManager_", "Singleton_1_ItemModule_",
    "Singleton_1_LoadingManager_", "Singleton_1_UIManager_1_",
    "Singleton_1_PlayerModule_", "Singleton_1_MapManager_",
    "Singleton_1_ScenePropManager_", "Singleton_1_NetworkManager_1_"
};

// Important method names to find
static std::vector<std::string> g_targetMethodNames = {
    "GetBaseCombat", "GetMoveComponent", "GetInstance", "SetItem",
    "GetAvatarPos", "SetAvatarPos", "GetAbsolutePosition", "SetAbsolutePosition",
    "GetCurrentAvatar", "GetEntities", "get_position", "set_position",
    "GetMainCameraEntity", "GetValidEntity", "SetPosition", "FireEvent",
    "GetForward", "GetRight", "GetUp", "IsActive", "ToStringRelease",
    "PickItem", "Update", "Tick", "DoHitEntity", "RequestSceneEntityMoveReq",
    "PerformPlayerTransmit", "SceneGoto", "CalcCurrentGroundHeight",
    "GenWorldPos", "GetBounds", "IsVisible", "IsInCamera"
};

static std::string TempPath(const char* name) {
    char buf[MAX_PATH];
    GetTempPathA(MAX_PATH, buf);
    std::string p(buf);
    if (!p.empty() && p.back() != '\\') p.push_back('\\');
    p += name;
    return p;
}

static void LogLine(const char* format, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    std::lock_guard<std::mutex> lk(g_logMtx);
    if (g_log) {
        std::fputs(buffer, g_log);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
    OutputDebugStringA((std::string("[GI Dumper] ") + buffer + "\n").c_str());
}

static void WriteFileImmediately(const std::string& filename, const std::string& content) {
    if (!g_options.enableRealTimeOutput) return;
    
    std::string fullPath = TempPath(filename.c_str());
    std::ofstream file(fullPath, std::ios::app);
    if (file.is_open()) {
        file << content << std::endl;
        file.flush();
        file.close();
    }
}

// ======= OPTIONS DISPLAY ======= 
static void DisplayCurrentOptions() {
    LogLine("=== Dumper huetenka blyat ===");
    LogLine("Total Dump: %s", g_options.enableTotalDump ? "ON" : "OFF");
    LogLine("Important Dump: %s", g_options.enableImportantDump ? "ON" : "OFF");
    LogLine("Vector Tracking: %s", g_options.enableVectorTracking ? "ON" : "OFF");
    LogLine("String Relations: %s", g_options.enableStringRelation ? "ON" : "OFF");
    LogLine("Real-time Output: %s", g_options.enableRealTimeOutput ? "ON" : "OFF");
    LogLine("Comprehensive Dump: %s", g_options.enableComprehensiveDump ? "ON" : "OFF");
    LogLine("Address Discovery: %s", g_options.enableAddressDiscovery ? "ON" : "OFF");
    LogLine("Console Monitoring: %s", g_options.enableConsoleMonitoring ? "ON" : "OFF");
    LogLine("Position Vector Tracking: %s", g_options.enablePositionVectorTracking ? "ON" : "OFF");
    LogLine("Damage Vector Tracking: %s", g_options.enableDamageVectorTracking ? "ON" : "OFF");
    LogLine("All String Dump: %s", g_options.enableAllStringDump ? "ON" : "OFF");
    LogLine("Related Strings Only: %s", g_options.enableRelatedStringsOnly ? "ON" : "OFF");
    LogLine("Important Strings Only: %s", g_options.enableImportantStringsOnly ? "ON" : "OFF");
    LogLine("======================");
}

// ======= MEMORY ACCESS =======
static bool FastReadMemory(HANDLE process, const void* address, void* buffer, size_t size) {
    if (!address || !buffer || size == 0) return false;
    
    SIZE_T bytesRead = 0;
    BOOL result = ReadProcessMemory(process, address, buffer, size, &bytesRead);
    return result && (bytesRead == size);
}

static bool FastReadPointer(HANDLE process, const void* base, int offset, void** result) {
    if (!base || !result) return false;
    const u8* ptrAddr = static_cast<const u8*>(base) + offset;
    return FastReadMemory(process, ptrAddr, result, sizeof(void*));
}

static bool FastReadString(HANDLE process, const void* strAddr, std::string& result, size_t maxLen = 200) {
    if (!strAddr) return false;
    
    char buffer[256];
    size_t chunkSize = (maxLen < 255) ? maxLen : 255;
    
    if (!FastReadMemory(process, strAddr, buffer, chunkSize + 1)) {
        return false;
    }
    
    buffer[chunkSize] = '\0';
    
    size_t len = 0;
    for (size_t i = 0; i < chunkSize; i++) {
        if (buffer[i] == '\0') {
            len = i;
            break;
        }
        if (buffer[i] < 32 || buffer[i] > 126) {
            return false;
        }
    }
    
    if (len == 0 || len > maxLen) return false;
    
    result.assign(buffer, len);
    return true;
}

static bool FastReadUInt16(HANDLE process, const void* address, uint16_t* result) {
    return FastReadMemory(process, address, result, sizeof(uint16_t));
}

static bool FastReadFloat(HANDLE process, const void* address, float* result) {
    return FastReadMemory(process, address, result, sizeof(float));
}

static bool FastReadVector3(HANDLE process, const void* address, Vector3& result) {
    float values[3];
    if (FastReadMemory(process, address, values, sizeof(values))) {
        result.x = values[0];
        result.y = values[1];
        result.z = values[2];
        return true;
    }
    return false;
}

// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!
// СТОП ЭТО КОМЕНТЫ?!?!??!?!?!?!?!?? И ОНИ КРАСИВО ОФОРМЛЕНЫ?!?!?!?!?!? АААААААААААААААААААА ЗНАЧИТ GROK!!!!!!!!!!!!!!!!

// ======= CONSOLE SETUP =======
static void SetupConsole() {
    if (!g_options.enableConsoleMonitoring) return;
    
    AllocConsole();
    g_consoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    
    FILE* pCout;
    freopen_s(&pCout, "CONOUT$", "w", stdout);
    
    SetConsoleTitle(L"Damper you know?");
    
    COORD coord;
    coord.X = 160;
    coord.Y = 50;
    SetConsoleScreenBufferSize(g_consoleHandle, coord);
    
    SMALL_RECT rect;
    rect.Top = 0;
    rect.Left = 0;
    rect.Bottom = 49;
    rect.Right = 159;
    SetConsoleWindowInfo(g_consoleHandle, TRUE, &rect);
}

static void UpdateConsoleDisplay() {
    if (!g_options.enableConsoleMonitoring || !g_consoleHandle) return;
    
    COORD coord = {0, 0};
    SetConsoleCursorPosition(g_consoleHandle, coord);
    
    std::cout << "Blya ya klanus rabotaet" << std::endl;
    std::cout << "Total: " << (g_options.enableTotalDump ? "ON" : "OFF")
              << " | Impotent: " << (g_options.enableImportantDump ? "ON" : "OFF")
              << " | Viktorina: " << (g_options.enableVectorTracking ? "ON" : "OFF")
              << " | Stringi: " << (g_options.enableStringRelation ? "ON" : "OFF") << std::endl;
    std::cout << "Class: " << g_classCount.load() 
              << " | Targetish " << g_targetClassCount.load()
              << " | DoksAdress: " << g_addressCount.load()
              << " | Stringi: " << g_stringCount.load() << std::endl;
    
    if (g_options.enableVectorTracking) {
        std::cout << "Vector Changes: " << g_vectorChangeCount.load()
                  << " | Tracked Vectors: " << g_allTrackedVectors.size() << std::endl;
    }
    
    std::cout << "================================================================" << std::endl;
    
    // Show discovered addresses based on enabled options
    if (g_options.enableAddressDiscovery && g_options.enableImportantDump) {
        std::cout << "DISCOVERED ADDRESSES (IMPOTENT):" << std::endl;
        std::lock_guard<std::mutex> lock(g_dataMutex);
        int count = 0;
        for (const auto& addr : g_allFoundAddresses) {
            if (count < 10 && addr.isImportant) {
                std::cout << "[" << (count + 1) << "] " << addr.type << ": " << addr.name 
                          << " @ 0x" << std::hex << addr.address << std::endl;
                std::cout << "    Class: " << addr.className;
                if (g_options.enableStringRelation && !addr.relatedStrings.empty()) {
                    std::cout << " | Related Strings: " << addr.relatedStrings.size();
                }
                std::cout << std::endl;
                count++;
            }
        }
    }
    
    // Show vector tracking if enabled
    if (g_options.enableVectorTracking) {
        std::cout << std::endl << "VIKTORINA TRACKING:" << std::endl;
        std::lock_guard<std::mutex> lock(g_dataMutex);
        int vectorCount = 0;
        for (const auto& vec : g_allTrackedVectors) {
            if (vectorCount < 5 && vec.changeCount > 0) {
                std::cout << "[" << (vectorCount + 1) << "] " << vec.className << "::" << vec.name 
                          << " @ 0x" << std::hex << vec.address << std::endl;
                std::cout << "    Current: " << vec.currentValue.toString() 
                          << " | Changes: " << vec.changeCount << std::endl;
                vectorCount++;
            }
        }
    }
    
    std::cout << "\nFiles written to: " << TempPath("") << std::endl;
}

// ======= STRING RELATION ANALYSIS =======
static void AnalyzeStringRelations(const std::string& str, void* stringAddr, 
                                  const std::string& className = "", const std::string& methodName = "") {
    if (!g_options.enableStringRelation) return;
    
    std::lock_guard<std::mutex> lock(g_dataMutex);
    
    // Add to string-to-class mapping
    if (!className.empty()) {
        g_stringToClassMap[str].push_back(className);
    }
    
    // Find related addresses
    for (auto& addr : g_allFoundAddresses) {
        if (str.find(addr.name) != std::string::npos || 
            addr.name.find(str) != std::string::npos ||
            str.find(addr.className) != std::string::npos) {
            addr.relatedStrings.push_back(str + " @ 0x" + 
                std::to_string(reinterpret_cast<uintptr_t>(stringAddr)));
        }
    }
    
    // Find related classes
    for (auto& classInfo : g_allClasses) {
        if (str.find(classInfo.name) != std::string::npos ||
            str.find(classInfo.fullName) != std::string::npos) {
            classInfo.relatedStrings.push_back(str + " @ 0x" + 
                std::to_string(reinterpret_cast<uintptr_t>(stringAddr)));
        }
    }
}

// ======= IL2CPP STRUCTURES =======
struct Il2CppFieldInfo {
    const char* name;
    void* type;
    void* parent;
    int32_t offset;
    uint32_t token;
};

struct Il2CppMethodInfo {
    void* methodPointer;
    void* invoker_method;
    const char* name;
    void* klass;
    void* return_type;
    void* parameters;
    void* rgctx_data;
    void* generic_container;
    uint32_t token;
    uint16_t flags;
    uint16_t iflags;
    uint16_t slot;
    uint8_t parameters_count;
};

// ======= ADDRESS DISCOVERY =======
static bool IsImportantMethod(const std::string& methodName) {
    for (const auto& targetMethod : g_targetMethodNames) {
        if (methodName.find(targetMethod) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static void RecordFoundAddress(const std::string& name, const std::string& className, 
                               const std::string& type, void* address, const std::string& signature) {
    if (!g_options.enableAddressDiscovery) return;
    
    FoundAddress foundAddr;
    foundAddr.name = name;
    foundAddr.className = className;
    foundAddr.type = type;
    foundAddr.address = address;
    foundAddr.signature = signature;
    foundAddr.isImportant = IsImportantMethod(name) || 
                           std::find(g_targetClassNames.begin(), g_targetClassNames.end(), className) != g_targetClassNames.end();
    
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        g_allFoundAddresses.push_back(foundAddr);
        g_addressCount++;
    }
    
    if (foundAddr.isImportant) {
        LogLine("[DOKS ADDRESS FOUND] %s::%s @ 0x%p (%s)", className.c_str(), name.c_str(), address, type.c_str());
        
        if (g_options.enableFileGrouping) {
            std::ostringstream addrOutput;
            addrOutput << "[" << type << "] " << className << "::" << name 
                      << " @ 0x" << std::hex << address << std::endl;
            addrOutput << "  Signature: " << signature << std::endl;
            WriteFileImmediately("Kitay_Kazik_addresses_" + type + ".txt", addrOutput.str());
        } else {
            std::ostringstream addrOutput;
            addrOutput << "[" << type << "] " << className << "::" << name 
                      << " @ 0x" << std::hex << address << std::endl;
            WriteFileImmediately("Kitay_Kazik_current_addresses.txt", addrOutput.str());
        }
    }
}

// ======= VECTOR TRACKING =======
static void InitializeVectorTracking(const ClassInfo& classInfo) {
    if (!g_options.enableVectorTracking) return;
    
    HANDLE hProcess = GetCurrentProcess();
    
    // Look for vector fields in this class
    for (size_t i = 0; i < classInfo.fields.size(); i++) {
        const auto& field = classInfo.fields[i];
        
        // Extract field name and offset
        size_t offsetPos = field.find("[offset: +0x");
        if (offsetPos != std::string::npos) {
            std::string fieldName = field.substr(0, offsetPos - 1);
            std::string offsetStr = field.substr(offsetPos + 12);
            size_t endPos = offsetStr.find("]");
            if (endPos != std::string::npos) {
                offsetStr = offsetStr.substr(0, endPos);
                
                try {
                    int offset = std::stoi(offsetStr, nullptr, 16);
                    void* fieldAddr = static_cast<u8*>(classInfo.address) + offset;
                    
                    // Check if it might be a vector
                    Vector3 testVector;
                    if (FastReadVector3(hProcess, fieldAddr, testVector)) {
                        // Check if values look reasonable for game coordinates
                        if (testVector.x >= -10000.0f && testVector.x <= 10000.0f &&
                            testVector.y >= -1000.0f && testVector.y <= 3000.0f &&
                            testVector.z >= -10000.0f && testVector.z <= 10000.0f) {
                            
                            TrackedVector tracker;
                            tracker.address = fieldAddr;
                            tracker.currentValue = testVector;
                            tracker.lastValue = testVector;
                            tracker.name = fieldName;
                            tracker.className = classInfo.fullName;
                            tracker.context = "Field vector in " + classInfo.fullName;
                            tracker.changeCount = 0;
                            tracker.lastUpdate = std::chrono::steady_clock::now();
                            
                            std::string lowerField = fieldName;
                            std::transform(lowerField.begin(), lowerField.end(), lowerField.begin(), ::tolower);
                            
                            tracker.isPositionVector = (lowerField.find("position") != std::string::npos ||
                                                       lowerField.find("pos") != std::string::npos ||
                                                       lowerField.find("transform") != std::string::npos ||
                                                       lowerField.find("location") != std::string::npos);
                                                       
                            tracker.isDamageVector = (lowerField.find("damage") != std::string::npos ||
                                                     lowerField.find("health") != std::string::npos ||
                                                     lowerField.find("hp") != std::string::npos);
                            
                            // Only track if we want this type of vector
                            if ((tracker.isPositionVector && g_options.enablePositionVectorTracking) ||
                                (tracker.isDamageVector && g_options.enableDamageVectorTracking) ||
                                (!tracker.isPositionVector && !tracker.isDamageVector)) {
                                
                                std::lock_guard<std::mutex> lock(g_dataMutex);
                                g_allTrackedVectors.push_back(tracker);
                                
                                LogLine("[VECTOR TRACK] %s::%s @ 0x%p: %s", 
                                    classInfo.fullName.c_str(), fieldName.c_str(), fieldAddr, 
                                    testVector.toString().c_str());
                            }
                        }
                    }
                } catch (...) {
                    // Invalid offset, skip
                }
            }
        }
    }
}

static void MonitorVectorChanges() {
    if (!g_options.enableVectorTracking) return;
    
    HANDLE hProcess = GetCurrentProcess();
    
    std::lock_guard<std::mutex> lock(g_dataMutex);
    
    for (auto& tracker : g_allTrackedVectors) {
        Vector3 newValue;
        if (FastReadVector3(hProcess, tracker.address, newValue)) {
            
            if (newValue != tracker.currentValue) {
                // Vector changed!
                tracker.lastValue = tracker.currentValue;
                tracker.currentValue = newValue;
                tracker.changeCount++;
                tracker.lastUpdate = std::chrono::steady_clock::now();
                g_vectorChangeCount++;
                
                std::ostringstream changeEntry;
                changeEntry << "[VECTOR CHANGE " << tracker.changeCount << "] " 
                           << tracker.className << "::" << tracker.name 
                           << " @ 0x" << std::hex << tracker.address << std::endl;
                changeEntry << "  From: " << tracker.lastValue.toString() 
                           << " To: " << tracker.currentValue.toString() << std::endl;
                changeEntry << "  Type: " << (tracker.isPositionVector ? "Position" : 
                                            tracker.isDamageVector ? "Damage" : "Unknown") << std::endl;
                
                tracker.changeHistory.push_back(changeEntry.str());
                
                if (g_options.enableVectorChangeLogging) {
                    WriteFileImmediately("Kitay_Kazik_vector_changes.txt", changeEntry.str());
                }
                
                LogLine("[VECTOR CHANGE] %s::%s: %s -> %s", 
                    tracker.className.c_str(), tracker.name.c_str(),
                    tracker.lastValue.toString().c_str(),
                    tracker.currentValue.toString().c_str());
            }
        }
    }
}

// ======= COMPREHENSIVE CLASS ANALYSIS =======
static void AnalyzeClassWithOptions(HANDLE process, void* classPtr, int classNumber) {
    if (!classPtr) return;
    
    void* namePtr = nullptr;
    void* nsPtr = nullptr;
    std::string name, nameSpace;
    
    if (!FastReadPointer(process, classPtr, kNameOff, &namePtr) || !namePtr ||
        !FastReadString(process, namePtr, name, 200) || name.length() < 2) {
        return;
    }
    
    FastReadPointer(process, classPtr, kNsOff, &nsPtr);
    if (nsPtr) {
        FastReadString(process, nsPtr, nameSpace, 200);
    }
    
    ClassInfo classInfo;
    classInfo.name = name;
    classInfo.nameSpace = nameSpace;
    classInfo.fullName = nameSpace.empty() ? name : (nameSpace + "::" + name);
    classInfo.address = classPtr;
    
    // Check if this is a target class
    bool isTarget = false;
    for (const auto& targetName : g_targetClassNames) {
        if (name.find(targetName) != std::string::npos || 
            classInfo.fullName.find(targetName) != std::string::npos) {
            classInfo.isTargetClass = true;
            isTarget = true;
            g_targetClassCount++;
            
            // Record class address
            RecordFoundAddress(name, classInfo.fullName, "Class", classPtr, 
                             "IL2CPP Class Structure realno @ " + std::to_string(reinterpret_cast<uintptr_t>(classPtr)));
            
            LogLine("[TARGET CLASS] Found: %s @ 0x%p", classInfo.fullName.c_str(), classPtr);
            break;
        }
    }
    
    // Skip non-target classes if only important dump is enabled
    if (g_options.enableImportantDump && !g_options.enableTotalDump && !isTarget) {
        return;
    }
    
    // Analyze fields
    uint16_t fieldCount = 0;
    void* fieldsPtr = nullptr;
    if (FastReadUInt16(process, static_cast<u8*>(classPtr) + kFieldCountOff, &fieldCount) &&
        fieldCount > 0 && fieldCount < 500 &&
        FastReadPointer(process, classPtr, kFieldsOff, &fieldsPtr) && fieldsPtr) {
        
        for (uint16_t i = 0; i < fieldCount; i++) {
            Il2CppFieldInfo fieldInfo;
            void* fieldAddr = static_cast<u8*>(fieldsPtr) + (i * sizeof(Il2CppFieldInfo));
            
            if (FastReadMemory(process, fieldAddr, &fieldInfo, sizeof(fieldInfo)) && fieldInfo.name) {
                std::string fieldName;
                if (FastReadString(process, fieldInfo.name, fieldName, 100)) {
                    
                    std::ostringstream fieldEntry;
                    fieldEntry << fieldName << " [offset: +0x" << std::hex << fieldInfo.offset << "]";
                    classInfo.fields.push_back(fieldEntry.str());
                    
                    // Record important field addresses
                    if (isTarget || g_options.enableTotalDump) {
                        std::ostringstream signature;
                        signature << "Field offset +0x" << std::hex << fieldInfo.offset << " in " << classInfo.fullName;
                        RecordFoundAddress(fieldName, classInfo.fullName, "Field", 
                                         static_cast<u8*>(classPtr) + fieldInfo.offset, signature.str());
                    }
                    
                    // Check for position/damage data
                    std::string lowerField = fieldName;
                    std::transform(lowerField.begin(), lowerField.end(), lowerField.begin(), ::tolower);
                    
                    if (lowerField.find("position") != std::string::npos ||
                        lowerField.find("pos") != std::string::npos ||
                        lowerField.find("transform") != std::string::npos ||
                        lowerField.find("location") != std::string::npos) {
                        classInfo.hasPositionData = true;
                    }
                    
                    if (lowerField.find("damage") != std::string::npos ||
                        lowerField.find("health") != std::string::npos ||
                        lowerField.find("hp") != std::string::npos) {
                        classInfo.hasDamageData = true;
                    }
                }
            }
        }
    }
    
    // Analyze methods
    uint16_t methodCount = 0;
    void* methodsPtr = nullptr;
    if (FastReadUInt16(process, static_cast<u8*>(classPtr) + kMethodCountOff, &methodCount) &&
        methodCount > 0 && methodCount < 1000 &&
        FastReadPointer(process, classPtr, kMethodsOff, &methodsPtr) && methodsPtr) {
        
        for (uint16_t i = 0; i < methodCount; i++) {
            void* methodPtr = nullptr;
            void* methodPtrAddr = static_cast<u8*>(methodsPtr) + (i * sizeof(void*));
            
            if (FastReadPointer(process, methodPtrAddr, 0, &methodPtr) && methodPtr) {
                Il2CppMethodInfo methodInfo;
                if (FastReadMemory(process, methodPtr, &methodInfo, sizeof(methodInfo)) && methodInfo.name) {
                    std::string methodName;
                    if (FastReadString(process, methodInfo.name, methodName, 100)) {
                        
                        std::ostringstream methodEntry;
                        methodEntry << methodName << "() [addr: 0x" << std::hex << methodInfo.methodPointer << "]";
                        classInfo.methods.push_back(methodEntry.str());
                        
                        // Record method addresses
                        if (IsImportantMethod(methodName) || isTarget || g_options.enableTotalDump) {
                            std::ostringstream signature;
                            signature << "Method in " << classInfo.fullName << " - IL2CPP MethodInfo @ 0x" 
                                     << std::hex << methodPtr << ", Code @ 0x" << methodInfo.methodPointer;
                            
                            RecordFoundAddress(methodName, classInfo.fullName, "Method", 
                                             methodInfo.methodPointer, signature.str());
                        }
                    }
                }
            }
        }
    }
    
    // Initialize vector tracking for this class
    InitializeVectorTracking(classInfo);
    
    // Store the class
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        g_allClasses.push_back(classInfo);
        
        if (classInfo.isTargetClass) {
            g_targetClasses[classInfo.fullName] = classInfo;
        }
        
        g_classCount++;
    }
    
    // Write output based on options
    if (g_options.enableRealTimeOutput && 
        ((g_options.enableTotalDump) || 
         (g_options.enableImportantDump && (isTarget || classInfo.hasPositionData || classInfo.hasDamageData)))) {
        
        std::ostringstream classOutput;
        classOutput << "[CLASS " << classNumber << "] " << classInfo.fullName << " @ 0x" << std::hex << classPtr << std::endl;
        classOutput << "  Target: " << (classInfo.isTargetClass ? "YES" : "NO")
                   << " | Position: " << (classInfo.hasPositionData ? "YES" : "NO")
                   << " | Damage: " << (classInfo.hasDamageData ? "YES" : "NO") << std::endl;
        
        if (g_options.enableTotalDump || isTarget) {
            classOutput << "  Fields (" << classInfo.fields.size() << "):" << std::endl;
            for (const auto& field : classInfo.fields) {
                classOutput << "    " << field << std::endl;
            }
            classOutput << "  Methods (" << classInfo.methods.size() << "):" << std::endl;
            for (const auto& method : classInfo.methods) {
                classOutput << "    " << method << std::endl;
            }
        }
        
        if (g_options.enableStringRelation && !classInfo.relatedStrings.empty()) {
            classOutput << "  Related Strings (" << classInfo.relatedStrings.size() << "):" << std::endl;
            for (const auto& str : classInfo.relatedStrings) {
                classOutput << "    " << str << std::endl;
            }
        }
        
        classOutput << std::endl;
        
        if (g_options.enableFileGrouping) {
            if (g_options.enableTotalDump) {
                WriteFileImmediately("Kitay_Kazik_total_dump.txt", classOutput.str());
            }
            if (g_options.enableImportantDump && (isTarget || classInfo.hasPositionData || classInfo.hasDamageData)) {
                WriteFileImmediately("Kitay_Kazik_important_dump.txt", classOutput.str());
            }
        } else {
            WriteFileImmediately("Kitay_Kazik_complete_analysis.txt", classOutput.str());
        }
    }
}

// ======= STRING EXTRACTION WITH OPTIONS =======
static void ExtractStringsWithOptions() {
    LogLine("[STRINGS] Extracting strings with options...");
    
    HANDLE hProcess = GetCurrentProcess();
    void* address = nullptr;
    MEMORY_BASIC_INFORMATION mbi;
    int totalStrings = 0;
    
    if (g_options.enableFileGrouping) {
        if (g_options.enableAllStringDump) {
            WriteFileImmediately("Kitay_Kazik_all_strings.txt", "BUTTERBRODSKY STRINGS!!!!!!!!!");
        }
        if (g_options.enableRelatedStringsOnly) {
            WriteFileImmediately("Kitay_Kazik_related_strings.txt", "MEN STRINGS!!!!!!!!!!");
        }
        if (g_options.enableImportantStringsOnly) {
            WriteFileImmediately("Kitay_Kazik_important_strings.txt", "WOMEN STRINGS!!!!!!!!!!!!!!!!");
        }
    }
    
    while (VirtualQueryEx(hProcess, address, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT && 
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE))) {
            
            const size_t chunkSize = 64 * 1024;
            
            for (size_t offset = 0; offset < mbi.RegionSize; offset += chunkSize) {
                void* chunkAddr = static_cast<u8*>(mbi.BaseAddress) + offset;
                size_t chunkSizeActual = (chunkSize < (mbi.RegionSize - offset)) ? 
                                        chunkSize : (mbi.RegionSize - offset);
                
                std::vector<u8> buffer(chunkSizeActual);
                if (FastReadMemory(hProcess, chunkAddr, buffer.data(), chunkSizeActual)) {
                    
                    for (size_t i = 0; i < chunkSizeActual - 3; i++) {
                        if (buffer[i] >= 32 && buffer[i] <= 126) {
                            size_t strLen = 0;
                            
                            for (size_t j = i; j < chunkSizeActual && j < i + 500; j++) {
                                if (buffer[j] == 0) {
                                    strLen = j - i;
                                    break;
                                }
                                if (buffer[j] < 32 || buffer[j] > 126) {
                                    break;
                                }
                            }
                            
                            if (strLen >= 3 && strLen <= 500) {
                                std::string str(reinterpret_cast<char*>(buffer.data() + i), strLen);
                                totalStrings++;
                                g_stringCount++;
                                
                                {
                                    std::lock_guard<std::mutex> lock(g_dataMutex);
                                    g_allStrings.push_back(str);
                                }
                                
                                void* stringAddr = static_cast<u8*>(chunkAddr) + i;
                                
                                // Analyze string relations
                                AnalyzeStringRelations(str, stringAddr);
                                
                                // Check string importance
                                bool isImportant = false;
                                bool isRelated = false;
                                
                                // Check against target classes
                                for (const auto& targetName : g_targetClassNames) {
                                    if (str.find(targetName) != std::string::npos) {
                                        isImportant = true;
                                        isRelated = true;
                                        break;
                                    }
                                }
                                
                                // Check against target methods
                                for (const auto& methodName : g_targetMethodNames) {
                                    if (str.find(methodName) != std::string::npos) {
                                        isImportant = true;
                                        isRelated = true;
                                        break;
                                    }
                                }
                                
                                // Check for position/damage/health strings
                                if (str.find("position") != std::string::npos ||
                                    str.find("Position") != std::string::npos ||
                                    str.find("damage") != std::string::npos ||
                                    str.find("Damage") != std::string::npos ||
                                    str.find("health") != std::string::npos ||
                                    str.find("Health") != std::string::npos ||
                                    str.find("Transform") != std::string::npos ||
                                    str.find("Entity") != std::string::npos ||
                                    str.find("Avatar") != std::string::npos ||
                                    str.find("Player") != std::string::npos ||
                                    str.find("Monster") != std::string::npos) {
                                    isImportant = true;
                                }
                                
                                std::ostringstream stringEntry;
                                stringEntry << "[STRING] @ 0x" << std::hex << stringAddr 
                                           << ": \"" << str << "\"";
                                
                                // Write to appropriate files based on options
                                if (g_options.enableAllStringDump) {
                                    WriteFileImmediately("Kitay_Kazik_all_strings.txt", stringEntry.str());
                                }
                                
                                if (g_options.enableRelatedStringsOnly && isRelated) {
                                    WriteFileImmediately("Kitay_Kazik_related_strings.txt", stringEntry.str());
                                }
                                
                                if (g_options.enableImportantStringsOnly && isImportant) {
                                    WriteFileImmediately("Kitay_Kazik_important_strings.txt", stringEntry.str());
                                    LogLine("[IMPORTANT STRING] @ 0x%p: \"%s\"", stringAddr, str.c_str());
                                }
                                
                                i += strLen;
                            }
                        }
                    }
                }
            }
        }
        
        address = static_cast<u8*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    
    LogLine("[STRINGS] Found %d total strings", totalStrings);
}

// ======= CLASS DISCOVERY WITH OPTIONS =======
static void ClassDiscoveryWithOptions() {
    LogLine("[DISCOVERY] Starting class discovery with options");
    DisplayCurrentOptions();
    
    HANDLE hProcess = GetCurrentProcess();
    void* address = nullptr;
    MEMORY_BASIC_INFORMATION mbi;
    int regionCount = 0;
    int totalClasses = 0;
    
    while (VirtualQueryEx(hProcess, address, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT && 
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE))) {
            
            regionCount++;
            
            LogLine("[DISCOVERY] Scanning region %d: 0x%p (%zu KB)", 
                regionCount, mbi.BaseAddress, mbi.RegionSize / 1024);
            
            int regionClasses = 0;
            
            for (size_t offset = 0; offset < mbi.RegionSize - 0x200; offset += 8) {
                void* candidate = static_cast<u8*>(mbi.BaseAddress) + offset;
                
                void* namePtr = nullptr;
                if (FastReadPointer(hProcess, candidate, kNameOff, &namePtr) && namePtr) {
                    
                    std::string name;
                    if (FastReadString(hProcess, namePtr, name, 100) && name.length() > 1) {
                        
                        // Filter out invalid strings
                        if (name.find("Content-") == std::string::npos &&
                            name.find("User-Agent") == std::string::npos &&
                            name.find("Accept") == std::string::npos &&
                            name.find("HTTP/") == std::string::npos &&
                            name.find("GET ") == std::string::npos &&
                            name.find("POST ") == std::string::npos &&
                            name.find("://") == std::string::npos) {
                            
                            regionClasses++;
                            totalClasses++;
                            
                            // Analyze class with options
                            AnalyzeClassWithOptions(hProcess, candidate, totalClasses);
                            
                            // Monitor vector changes periodically
                            if (totalClasses % 100 == 0 && g_options.enableVectorTracking) {
                                MonitorVectorChanges();
                            }
                            
                            if (totalClasses % 1000 == 0) {
                                LogLine("=== PROGRESS: %d classes analyzed ===", totalClasses);
                                if (g_options.enableConsoleMonitoring) {
                                    UpdateConsoleDisplay();
                                }
                            }
                        }
                    }
                }
            }
            
            LogLine("[DISCOVERY] Region %d: Found %d classes (Total: %d)", 
                regionCount, regionClasses, totalClasses);
        }
        
        address = static_cast<u8*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    
    LogLine("[DISCOVERY] COMPLETE! Total classes: %d, Target classes: %d", 
        totalClasses, g_targetClassCount.load());
}

// ======= FINAL REPORTS =======
static void GenerateOptionsBasedReports() {
    LogLine("[OUTPUT] Generating reports based on enabled options...");
    
    std::string basePath = TempPath("");
    
    // Generate summary based on enabled options
    {
        std::ofstream summaryFile(basePath + "Kitay_Kazik_dumper_summary.txt");
        if (summaryFile.is_open()) {
            summaryFile << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" << std::endl;
            summaryFile << "Total Classes Analyzed: " << g_classCount.load() << std::endl;
            summaryFile << "Target Classes Found: " << g_targetClassCount.load() << std::endl;
            summaryFile << "Addresses Discovered: " << g_addressCount.load() << std::endl;
            summaryFile << "Total Strings: " << g_stringCount.load() << std::endl;
            
            if (g_options.enableVectorTracking) {
                summaryFile << "Vector Changes Detected: " << g_vectorChangeCount.load() << std::endl;
                summaryFile << "Vectors Tracked: " << g_allTrackedVectors.size() << std::endl;
            }
            
            summaryFile << std::endl << "ENABLED OPTIONS:" << std::endl;
            summaryFile << "Total Dump: " << (g_options.enableTotalDump ? "ON" : "OFF") << std::endl;
            summaryFile << "Important Dump: " << (g_options.enableImportantDump ? "ON" : "OFF") << std::endl;
            summaryFile << "Vector Tracking: " << (g_options.enableVectorTracking ? "ON" : "OFF") << std::endl;
            summaryFile << "String Relations: " << (g_options.enableStringRelation ? "ON" : "OFF") << std::endl;
            summaryFile << "Address Discovery: " << (g_options.enableAddressDiscovery ? "ON" : "OFF") << std::endl;
            
            summaryFile << std::endl << "FILES GENERATED:" << std::endl;
            if (g_options.enableTotalDump) {
                summaryFile << "- Kitay_Kazik_total_dump.txt (Complete class dump)" << std::endl;
            }
            if (g_options.enableImportantDump) {
                summaryFile << "- Kitay_Kazik_important_dump.txt (Target classes and position/damage data)" << std::endl;
            }
            if (g_options.enableVectorTracking) {
                summaryFile << "- Kitay_Kazik_vector_changes.txt (Vector position changes)" << std::endl;
            }
            if (g_options.enableStringRelation) {
                summaryFile << "- Kitay_Kazik_related_strings.txt (Strings related to classes/methods)" << std::endl;
            }
            if (g_options.enableAddressDiscovery && g_options.enableFileGrouping) {
                summaryFile << "- Kitay_Kazik_addresses_Class.txt (Class addresses)" << std::endl;
                summaryFile << "- Kitay_Kazik_addresses_Method.txt (Method addresses)" << std::endl;
                summaryFile << "- Kitay_Kazik_addresses_Field.txt (Field addresses)" << std::endl;
            }
            
            summaryFile.close();
        }
    }
    
    LogLine("[OUTPUT] Reports generated in: %s", basePath.c_str());
}

// ======= ÁËßÒÜ ÍÓ È ÕÓÉÍß =======
static DWORD WINAPI LiveMonitoringThread(LPVOID) {
    if (!g_options.enableConsoleMonitoring) return 0;
    
    SetupConsole();
    
    while (g_liveMonitoring) {
        UpdateConsoleDisplay();
        
        // Monitor vector changes if enabled
        if (g_options.enableVectorTracking) {
            MonitorVectorChanges();
        }
        
        Sleep(1000);
    }
    
    return 0;
}

DWORD WINAPI Run(LPVOID) {
    std::string outPath = TempPath("Kitay_Kazik_configurable_dumper.txt");
    if (fopen_s(&g_log, outPath.c_str(), "wb") != 0) {
        g_log = nullptr;
        OutputDebugStringA("[Kitay_Kazik Dumper] Failed to open log file\n");
        return 0;
    }
    
    LogLine("Blya ya ustal pisat");
    LogLine("Multiple dump modes with configurable options");
    DisplayCurrentOptions();
    LogLine("");

    if (g_options.enableConsoleMonitoring) {
        g_liveMonitoring = true;
        HANDLE monitorThread = CreateThread(NULL, 0, LiveMonitoringThread, NULL, 0, NULL);
    }
    
    if (g_options.enableFileGrouping) {
        if (g_options.enableTotalDump) {
            WriteFileImmediately("Kitay_Kazik_total_dump.txt", "EBAT PIZDEC");
        }
        if (g_options.enableImportantDump) {
            WriteFileImmediately("Kitay_Kazik_important_dump.txt", "IMPOTENT DUMP");
        }
    }
   
    ExtractStringsWithOptions();
    LogLine("");

    ClassDiscoveryWithOptions();
    LogLine("");

    GenerateOptionsBasedReports();
    
    LogLine("");
    LogLine("=== CONFIGURABLE LSD DUMPER COMPLETE ===");
    LogLine("Total classes analyzed: %d", g_classCount.load());
    LogLine("Target classes found: %d", g_targetClassCount.load());
    LogLine("Addresses discovered: %d", g_addressCount.load());
    LogLine("Total strings: %d", g_stringCount.load());
    
    if (g_options.enableVectorTracking) {
        LogLine("Vector changes detected: %d", g_vectorChangeCount.load());
        LogLine("Vectors tracked: %zu", g_allTrackedVectors.size());
    }
    
    LogLine("");
    LogLine("Files location: %s", TempPath("").c_str());
    LogLine("Dumper completed with selected options!");
    
    // Keep monitoring if enabled
    if (g_options.enableConsoleMonitoring && g_liveMonitoring) {
        for (int i = 0; i < 600; i++) { // 10 ìèíóò ÷òîá âñå ñîñàëè êòî æäåò
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (i % 60 == 0) {
                LogLine("[STATUS] Monitoring active... %d/600 seconds", i);
            }
        }
        
        g_liveMonitoring = false;
    }
    
    return 0;

}
