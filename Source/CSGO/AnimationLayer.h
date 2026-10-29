#pragma once

#include <cstddef>

#include <Platform/Macros/PlatformSpecific.h>

namespace csgo
{

// Слоёв у игрока ровно 13 (цикл в CCSGOPlayerAnimState::Update: cmp $0xd)
constexpr int ANIMATION_LAYER_COUNT = 13;

// Имена из движка — для логов резольвера
enum AnimationLayerIndex {
    LAYER_AIMMATRIX = 0,
    LAYER_WEAPON_ACTION,
    LAYER_WEAPON_ACTION_RECROUCH,
    LAYER_ADJUST,                   // доворот ног к взгляду — главный сигнал для резольвера
    LAYER_MOVEMENT_JUMP_OR_FALL,
    LAYER_MOVEMENT_LAND_OR_CLIMB,
    LAYER_MOVEMENT_MOVE,
    LAYER_MOVEMENT_STRAFECHANGE,
    LAYER_WHOLE_BODY,
    LAYER_FLASHED,
    LAYER_FLINCH,
    LAYER_ALIVELOOP,
    LAYER_LEAN
};

// CAnimationLayer. Офсеты — из recv-таблицы DT_Animationlayer (client_client.so, 0x7b10c0)
// и C_BaseAnimatingOverlay::GetAnimOverlay (0x7b1450): шаг массива 0x48, m_nOrder на 0x18.
struct AnimationLayer {
    std::byte pad0[0x18];
    int order;              // 0x18
    int sequence;           // 0x1c
    float prevCycle;        // 0x20
    float weight;           // 0x24
    float weightDeltaRate;  // 0x28
    float playbackRate;     // 0x2c
    float cycle;            // 0x30
    std::byte pad1[0x4];
    void* owner;            // 0x38
    std::byte pad2[0x8];
};

#if IS_LINUX()
static_assert(sizeof(AnimationLayer) == 0x48);
static_assert(offsetof(AnimationLayer, order) == 0x18);
static_assert(offsetof(AnimationLayer, cycle) == 0x30);
#endif

// m_AnimOverlay — CUtlVector<CAnimationLayer> в C_BaseAnimatingOverlay
// Офсет CUtlVector<CAnimationLayer> m_AnimOverlay в игроке (linux — из GetAnimOverlay, 0 — не найден)
inline int animOverlayOffset = 0;

struct AnimationLayerVector {
    AnimationLayer* memory;
    int allocationCount;
    int growSize;
    int size;
    int pad;
    AnimationLayer* elements;
};

}
