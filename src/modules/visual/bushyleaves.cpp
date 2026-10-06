#include "bushyleaves.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/events/EventBus.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace {

struct Vec3Raw { float x, y, z; };
using FaceFn = void (*)(void*, void*, const void*, const Vec3Raw*, const void*);
using SetAllDirtyFn = void (*)(void*, bool, bool);

std::atomic_bool g_enabled{false};
std::atomic_int g_layers{5};
std::atomic<float> g_spread{0.10f};
std::atomic_bool g_allLeaves{true};
std::atomic_bool g_topBottom{true};
std::atomic_bool g_sides{true};
std::atomic_bool g_rebuildPending{false};

struct FaceHook {
    bedrocktools::hooks::Handle handle = nullptr;
    FaceFn original = nullptr;
};

FaceHook g_faceHooks[6]{};
SetAllDirtyFn g_setAllDirty = nullptr;

constexpr std::array<bedrocktools::memory::SignatureId, 6> kFaceSigIds = {
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceDown,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceUp,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceNorth,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceSouth,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceWest,
    bedrocktools::memory::SignatureId::BlockTessellatorTessellateFaceEast,
};

enum class Face : uint8_t { Down = 0, Up, North, South, West, East };

std::string_view stripNamespace(std::string_view name) {
    constexpr std::string_view prefix = "minecraft:";
    if (name.starts_with(prefix)) name.remove_prefix(prefix.size());
    return name;
}

bool isLeafBlock(const void* block) {
    if (!block) return false;
    const auto* b = static_cast<const bedrocktools::sdk::Block*>(block);
    const auto* fullName = b->fullName();
    if (!fullName || fullName->empty() || fullName->size() > 128) return false;

    const std::string_view name = stripNamespace({fullName->data(), fullName->size()});
    if (name == "leaves" || name == "azalea_leaves" || name == "flowering_azalea_leaves") return true;
    return name.ends_with("_leaves");
}

bool faceEnabled(Face face) {
    if (face == Face::Down || face == Face::Up)
        return g_topBottom.load(std::memory_order_relaxed);
    return g_sides.load(std::memory_order_relaxed);
}

std::array<Vec3Raw, 9> offsetsFor(Face face, int layers, float spread) {
    std::array<Vec3Raw, 9> out{};
    out[0] = {0.0f, 0.0f, 0.0f};

    // Tangent-plane offsets make the leaf face appear as a soft, irregular volume.
    Vec3Raw a{}, b{};
    switch (face) {
        case Face::Down:
        case Face::Up:
            a = {spread, 0.0f, 0.0f};
            b = {0.0f, 0.0f, spread};
            break;
        case Face::North:
        case Face::South:
            a = {spread, 0.0f, 0.0f};
            b = {0.0f, spread, 0.0f};
            break;
        case Face::West:
        case Face::East:
            a = {0.0f, 0.0f, spread};
            b = {0.0f, spread, 0.0f};
            break;
    }

    out[1] = a;
    out[2] = {-a.x, -a.y, -a.z};
    out[3] = b;
    out[4] = {-b.x, -b.y, -b.z};
    out[5] = {a.x + b.x, a.y + b.y, a.z + b.z};
    out[6] = {-a.x - b.x, -a.y - b.y, -a.z - b.z};
    out[7] = {a.x - b.x, a.y - b.y, a.z - b.z};
    out[8] = {-a.x + b.x, -a.y + b.y, -a.z + b.z};

    if (layers <= 1) {
        for (size_t i = 1; i < out.size(); ++i) out[i] = out[0];
    } else if (layers <= 3) {
        for (size_t i = 3; i < out.size(); ++i) out[i] = out[0];
    } else if (layers <= 5) {
        for (size_t i = 5; i < out.size(); ++i) out[i] = out[0];
    }
    return out;
}

void renderBushyFace(FaceHook& hook, Face face, void* tess, void* meshTess,
                     const void* block, const Vec3Raw* position, const void* texture) {
    if (!hook.original) return;

    // Always render the game's original geometry first.
    hook.original(tess, meshTess, block, position, texture);

    if (!g_enabled.load(std::memory_order_relaxed) || !faceEnabled(face)
        || !tess || !block || !position || !texture || !g_allLeaves.load(std::memory_order_relaxed)
        || !isLeafBlock(block)) {
        return;
    }

    const int layers = std::clamp(g_layers.load(std::memory_order_relaxed), 1, 9);
    const float spread = std::clamp(g_spread.load(std::memory_order_relaxed), 0.01f, 0.30f);
    const auto offsets = offsetsFor(face, layers, spread);

    // Re-submit the same textured face at small tangent offsets. Bedrock's normal
    // block tessellator then supplies the correct leaf material/UVs for every copy.
    const int copies = layers;
    for (int i = 1; i < copies; ++i) {
        Vec3Raw p = {
            position->x + offsets[static_cast<size_t>(i)].x,
            position->y + offsets[static_cast<size_t>(i)].y,
            position->z + offsets[static_cast<size_t>(i)].z,
        };
        hook.original(tess, meshTess, block, &p, texture);
    }
}

template <Face F>
void faceHookTrampoline(void* a0, void* a1, const void* a2, const Vec3Raw* a3, const void* a4) {
    renderBushyFace(g_faceHooks[static_cast<size_t>(F)], F, a0, a1, a2, a3, a4);
}

constexpr std::array<FaceFn, 6> kTrampolines = {
    &faceHookTrampoline<Face::Down>,
    &faceHookTrampoline<Face::Up>,
    &faceHookTrampoline<Face::North>,
    &faceHookTrampoline<Face::South>,
    &faceHookTrampoline<Face::West>,
    &faceHookTrampoline<Face::East>,
};

template <typename Fn>
bedrocktools::hooks::Handle installHook(bedrocktools::memory::SignatureId id, void* detour, Fn* original) {
    const uintptr_t address = bedrocktools::memory::resolve(id);
    if (!address) return nullptr;
    return bedrocktools::hooks::install(reinterpret_cast<void*>(address), detour,
                                        reinterpret_cast<void**>(original));
}

bool rebuildRenderChunks(void* clientInstance) {
    if (!clientInstance || !g_setAllDirty) return false;

    void* levelRenderer = bedrocktools::sdk::field<void*>(
        clientInstance, bedrocktools::sdk::offsets::ClientInstance::mLevelRenderer);
    if (!levelRenderer) return false;

    void* node = bedrocktools::sdk::field<void*>(
        levelRenderer,
        bedrocktools::sdk::offsets::LevelRenderer::mRenderChunkCoordinators
            + bedrocktools::sdk::offsets::HashTable::mFirstNode);

    bool rebuilt = false;
    size_t visited = 0;
    while (node && visited++ < bedrocktools::sdk::offsets::RenderChunkCoordinator::MaxNodes) {
        void* next = bedrocktools::sdk::field<void*>(
            node, bedrocktools::sdk::offsets::HashNode::mNext);
        void* coordinator = bedrocktools::sdk::field<void*>(
            node, bedrocktools::sdk::offsets::HashNode::mValuePointer);
        if (coordinator) {
            g_setAllDirty(coordinator, true, false);
            rebuilt = true;
        }
        node = next;
    }
    return rebuilt;
}

} // namespace

void BushyLeavesHandleClientInstanceUpdate(void* clientInstance) {
    if (!g_rebuildPending.load(std::memory_order_acquire)) return;
    if (rebuildRenderChunks(clientInstance))
        g_rebuildPending.store(false, std::memory_order_release);
}

BushyLeavesModule::BushyLeavesModule()
    : Module("Bushy Leaves", "Adds layered, fluffy geometry to leaf blocks using the native block tessellator.") {}

BushyLeavesModule::~BushyLeavesModule() {
    g_enabled.store(false, std::memory_order_relaxed);
}

void BushyLeavesModule::applySettings() {
    layers = std::clamp(layers, 1, 9);
    spread = std::clamp(spread, 0.01f, 0.30f);
    g_layers.store(layers, std::memory_order_relaxed);
    g_spread.store(spread, std::memory_order_relaxed);
    g_allLeaves.store(allLeaves, std::memory_order_relaxed);
    g_topBottom.store(topBottom, std::memory_order_relaxed);
    g_sides.store(sides, std::memory_order_relaxed);
}

void BushyLeavesModule::installHooks() {
    if (m_hooked) return;

    g_setAllDirty = reinterpret_cast<SetAllDirtyFn>(
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::RenderChunkCoordinatorSetAllDirty));

    for (size_t i = 0; i < 6; ++i) {
        if (!g_faceHooks[i].handle) {
            g_faceHooks[i].handle = installHook(
                kFaceSigIds[i], reinterpret_cast<void*>(kTrampolines[i]),
                &g_faceHooks[i].original);
        }
    }

    m_hooked = g_setAllDirty != nullptr;
    for (const auto& h : g_faceHooks) m_hooked = m_hooked && h.handle;
}

void BushyLeavesModule::onInit() {
    applySettings();
    installHooks();
    bedrocktools::events::bus().subscribe<bedrocktools::events::ClientInstanceUpdateEvent>([](auto& event) {
        BushyLeavesHandleClientInstanceUpdate(event.clientInstance);
    });
}

void BushyLeavesModule::onEnable() {
    applySettings();
    if (!m_hooked) installHooks();
    g_enabled.store(m_hooked, std::memory_order_release);
    if (m_hooked) g_rebuildPending.store(true, std::memory_order_release);
}

void BushyLeavesModule::onDisable() {
    g_enabled.store(false, std::memory_order_release);
    if (m_hooked) g_rebuildPending.store(true, std::memory_order_release);
}

void BushyLeavesModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    allLeaves = j.value("allLeaves", allLeaves);
    includeTransparentLeaves = j.value("includeTransparentLeaves", includeTransparentLeaves);
    layers = j.value("layers", layers);
    spread = j.value("spread", spread);
    topBottom = j.value("topBottom", topBottom);
    sides = j.value("sides", sides);
    applySettings();
    if (enabled && m_hooked) g_rebuildPending.store(true, std::memory_order_release);
}

void BushyLeavesModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["allLeaves"] = allLeaves;
    j["includeTransparentLeaves"] = includeTransparentLeaves;
    j["layers"] = layers;
    j["spread"] = spread;
    j["topBottom"] = topBottom;
    j["sides"] = sides;
}
