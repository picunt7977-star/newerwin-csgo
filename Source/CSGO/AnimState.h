#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <Platform/Macros/PlatformSpecific.h>

namespace csgo
{

struct EntityPOD;
struct Vector;

#if IS_WIN32()

// Старая раскладка под 32-битную сборку (офсеты из Osiris) — на Windows резольвер по слоям выключен
struct AnimState {
    std::byte pad[164];
    float duckAmount;
    std::byte pad1[80];
    float walkSpeedPortion;
    float crouchSpeedPortion;
    std::byte pad2[22];
    float stopToFullRunningFraction;
    std::byte pad3[532];
    float maxBodyYawDegrees;

    [[nodiscard]] float getMaxDesyncAngle() const noexcept
    {
        float modifier = (stopToFullRunningFraction * -0.3f - 0.2f) * std::clamp(walkSpeedPortion, 0.0f, 1.0f) + 1.0f;
        if (duckAmount > 0.0f)
            modifier += duckAmount * std::clamp(crouchSpeedPortion, 0.0f, 1.0f) * (0.5f - modifier);
        return maxBodyYawDegrees * modifier;
    }
};

#else

// CCSGOPlayerAnimState, linux64-сборка csgo legacy (клиент 13546): размер 0x3b8.
// Офсеты вытащены из client_client.so: Update (0xf45560), ComputePoseParam_BodyYaw (0xf428b2),
// CachePoseParams (0xf431f0) и CreateAnimationState (0xf3eef0 — там же размер `new 0x3b8`).
struct AnimState {
    std::byte pad0[0x68];
    EntityPOD* player;                  // 0x68
    std::byte pad1[0x10];
    float lastUpdateTime;               // 0x80  последний curtime, когда анимация считалась
    int lastUpdateFrame;                // 0x84  и номер кадра — по ним Update отсекает повторный вызов
    float updateDelta;                  // 0x88  время с прошлого обновления
    float eyeYaw;                       // 0x8c  куда смотрит (нормализованный аргумент Update)
    float pitch;                        // 0x90
    float footYaw;                      // 0x94  угол ног — то, что ищет резольвер
    float footYawLast;                  // 0x98  он же на прошлом обновлении
    std::byte pad2[0x1c];
    float duckAmount;                   // 0xb8
    std::byte pad3[0x8];
    float velocityX;                    // 0xc4
    float velocityY;                    // 0xc8
    float velocityZ;                    // 0xcc
    std::byte pad4[0x3c];
    float walkSpeedPortion;             // 0x10c доля от максимальной скорости бега
    float crouchSpeedPortion;           // 0x110 то же для приседа
    std::byte pad5[0x1c];
    float stopToFullRunningFraction;    // 0x130
    std::byte pad6[0x274];
    float maxBodyYawDegrees;            // 0x3a8 обычно 58
    std::byte pad7[0xc];

    // Как считает сам клиент (f426ac): чем быстрее бежит и чем глубже присел, тем меньше десинк
    [[nodiscard]] float getMaxDesyncAngle() const noexcept
    {
        float modifier = (stopToFullRunningFraction * -0.3f - 0.2f) * std::clamp(walkSpeedPortion, 0.0f, 1.0f) + 1.0f;
        if (duckAmount > 0.0f)
            modifier += duckAmount * std::clamp(crouchSpeedPortion, 0.0f, 1.0f) * (0.5f - modifier);
        return maxBodyYawDegrees * modifier;
    }
};

static_assert(sizeof(AnimState) == 0x3b8);
static_assert(offsetof(AnimState, lastUpdateTime) == 0x80);
static_assert(offsetof(AnimState, eyeYaw) == 0x8c);
static_assert(offsetof(AnimState, footYaw) == 0x94);
static_assert(offsetof(AnimState, duckAmount) == 0xb8);
static_assert(offsetof(AnimState, velocityX) == 0xc4);
static_assert(offsetof(AnimState, walkSpeedPortion) == 0x10c);
static_assert(offsetof(AnimState, stopToFullRunningFraction) == 0x130);
static_assert(offsetof(AnimState, maxBodyYawDegrees) == 0x3a8);

#endif

// Офсет m_pPlayerAnimState в C_CSPlayer. Memory перезаписывает его найденным по паттерну значением.
inline int animStateOffset = WIN32_LINUX(0x3914, 0xa250);

}
