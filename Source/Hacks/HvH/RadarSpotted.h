#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include <CSGO/Vector.h>

// Позиции с радара. Игроков вне нашей видимости (PVS) сервер не присылает — entity дормант, ESP пропадает.
// Но тех, кого заметила наша команда, сервер шлёт для радара отдельным сообщением
// CCSUsrMsg_ProcessSpottedEntityUpdate (номер 25 в ECstrike15UserMessages): индекс игрока и координаты.
// По ним дормант-ESP держит врага на месте, где его видят союзники.
namespace hvh::radar
{

constexpr int spottedEntityUpdate = 25;

struct Entry {
    int x = 0, y = 0, z = 0;    // как пришло (сервер может делить координаты на 4)
    bool haveZ = false;
    float at = 0.0f;
};
inline std::array<Entry, 65> entries{};
inline float scale = 4.0f;      // уточняется по живым игрокам: сравниваем с настоящим origin

[[nodiscard]] inline bool readVarint(const std::uint8_t*& p, const std::uint8_t* end, std::uint64_t& value) noexcept
{
    value = 0;
    for (int shift = 0; shift < 64 && p < end; shift += 7) {
        const std::uint8_t byte = *p++;
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80))
            return true;
    }
    return false;
}

// пропустить поле неизвестного номера по его wire type
[[nodiscard]] inline bool skipField(const std::uint8_t*& p, const std::uint8_t* end, int wireType) noexcept
{
    std::uint64_t value;
    switch (wireType) {
    case 0: return readVarint(p, end, value);
    case 1: p += 8; return p <= end;
    case 2: if (!readVarint(p, end, value) || value > static_cast<std::uint64_t>(end - p)) return false; p += value; return true;
    case 5: p += 4; return p <= end;
    default: return false;
    }
}

inline void parseUpdate(const std::uint8_t* p, const std::uint8_t* end, float realtime) noexcept
{
    int index = -1;
    std::int64_t origin[3]{};
    bool haveOrigin[3]{};
    while (p < end) {
        std::uint64_t key;
        if (!readVarint(p, end, key))
            return;
        const int field = static_cast<int>(key >> 3), wireType = static_cast<int>(key & 7);
        if (wireType == 0 && (field == 1 || (field >= 3 && field <= 5))) {
            std::uint64_t value;
            if (!readVarint(p, end, value))
                return;
            // int32: отрицательные идут 10-байтовым varint в дополнительном коде
            const auto signedValue = static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
            if (field == 1)
                index = signedValue;
            else {
                origin[field - 3] = signedValue;
                haveOrigin[field - 3] = true;
            }
        } else if (!skipField(p, end, wireType)) {
            return;
        }
    }
    if (index <= 0 || index >= static_cast<int>(entries.size()) || !haveOrigin[0] || !haveOrigin[1])
        return;
    entries[index] = { static_cast<int>(origin[0]), static_cast<int>(origin[1]), static_cast<int>(origin[2]), haveOrigin[2], realtime };
}

inline void onUserMessage(const void* data, int size, float realtime) noexcept
{
    if (!data || size <= 0)
        return;
    auto p = static_cast<const std::uint8_t*>(data);
    const auto end = p + size;
    while (p < end) {
        std::uint64_t key;
        if (!readVarint(p, end, key))
            return;
        const int field = static_cast<int>(key >> 3), wireType = static_cast<int>(key & 7);
        if (field == 2 && wireType == 2) {
            std::uint64_t length;
            if (!readVarint(p, end, length) || length > static_cast<std::uint64_t>(end - p))
                return;
            parseUpdate(p, p + length, realtime);
            p += length;
        } else if (!skipField(p, end, wireType)) {
            return;
        }
    }
}

[[nodiscard]] inline csgo::Vector toWorld(const Entry& entry) noexcept
{
    return { entry.x * scale, entry.y * scale, entry.z * scale };
}

// живой (не дормант) игрок: по нему проверяем, делит ли сервер координаты на 4
inline void calibrate(int index, const csgo::Vector& origin, float realtime) noexcept
{
    if (index <= 0 || index >= static_cast<int>(entries.size()))
        return;
    const auto& entry = entries[index];
    if (entry.at <= 0.0f || realtime - entry.at > 0.3f || std::abs(origin.x) + std::abs(origin.y) < 64.0f)
        return;
    const auto distance = [&](float s) {
        return std::hypot(entry.x * s - origin.x, entry.y * s - origin.y);
    };
    scale = distance(1.0f) < distance(4.0f) ? 1.0f : 4.0f;
}

// позиция с радара новее since; высоты может не быть — тогда остаётся прежняя (out.z не трогаем)
[[nodiscard]] inline bool origin(int index, float since, csgo::Vector& out) noexcept
{
    if (index <= 0 || index >= static_cast<int>(entries.size()))
        return false;
    const auto& entry = entries[index];
    if (entry.at <= since)
        return false;
    const auto world = toWorld(entry);
    out.x = world.x;
    out.y = world.y;
    if (entry.haveZ)
        out.z = world.z;
    return true;
}

inline void clear() noexcept
{
    entries = {};
}

}
