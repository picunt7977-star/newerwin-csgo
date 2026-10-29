#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include "ModelPreview.h"

#include <Helpers.h>
#include <Texture.h>
#include <Memory.h>
#include <Interfaces/ClientInterfaces.h>
#include <Interfaces/EngineInterfaces.h>
#include <CSGO/Engine.h>
#include <CSGO/Entity.h>
#include <CSGO/EntityList.h>
#include <CSGO/GlobalVars.h>
#include <CSGO/LocalPlayer.h>
#include <CSGO/matrix3x4.h>
#include <CSGO/ModelInfo.h>
#include <CSGO/ModelRender.h>
#include <RetSpoof/RetSpoofGadgets.h>

namespace hvh::preview
{

namespace
{

// ---------- чтение файлов из VPK игры (pak01_dir.vpk + архивы), без API движка ----------

struct VpkEntry {
    std::uint16_t archive;
    std::uint32_t offset;
    std::uint32_t length;
    std::string preload;
};

class Vpk {
public:
    bool open() noexcept
    {
        // /proc/self/exe = .../csgo legacy/csgo_linux64
        char exe[4096]{};
        const auto length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (length <= 0)
            return false;
        std::string dir{ exe, static_cast<std::size_t>(length) };
        dir.resize(dir.rfind('/') + 1);
        base = dir + "csgo/pak01_";

        std::ifstream file{ base + "dir.vpk", std::ios::binary };
        if (!file)
            return false;
        std::uint32_t header[3]{};
        file.read(reinterpret_cast<char*>(header), sizeof(header));
        if (header[0] != 0x55AA1234)
            return false;
        if (header[1] == 2)
            file.seekg(16, std::ios::cur);
        const auto treeStart = static_cast<std::uint32_t>(file.tellg());
        dataStart = treeStart + header[2];

        const auto readString = [&file]() {
            std::string s;
            std::getline(file, s, '\0');
            return s;
        };

        // нужны только модели игроков — остальное пропускаем, чтобы не держать 130 тыс. строк
        while (file) {
            const auto extension = readString();
            if (extension.empty())
                break;
            while (file) {
                const auto path = readString();
                if (path.empty())
                    break;
                // модели игроков и их материалы (перчатки агентов лежат у оружия)
                const bool wanted = path.starts_with("models/player") || path.starts_with("materials/models/player")
                    || path.starts_with("materials/models/weapons/w_models/arms");
                while (file) {
                    const auto name = readString();
                    if (name.empty())
                        break;
                    std::uint8_t raw[18];
                    file.read(reinterpret_cast<char*>(raw), sizeof(raw));
                    std::uint16_t preloadBytes, archive;
                    std::uint32_t offset, size;
                    std::memcpy(&preloadBytes, raw + 4, 2);
                    std::memcpy(&archive, raw + 6, 2);
                    std::memcpy(&offset, raw + 8, 4);
                    std::memcpy(&size, raw + 12, 4);
                    std::string preload(preloadBytes, '\0');
                    file.read(preload.data(), preloadBytes);
                    if (wanted)
                        entries[path + '/' + name + '.' + extension] = { archive, offset, size, std::move(preload) };
                }
            }
        }
        return !entries.empty();
    }

    [[nodiscard]] std::string read(const std::string& name) const noexcept
    {
        const auto it = entries.find(name);
        if (it == entries.end())
            return {};
        const auto& entry = it->second;
        std::string data = entry.preload;
        if (entry.length == 0)
            return data;

        char archiveName[8];
        std::snprintf(archiveName, sizeof(archiveName), "%03u", entry.archive);
        std::ifstream file{ entry.archive == 0x7FFF ? base + "dir.vpk" : base + archiveName + ".vpk", std::ios::binary };
        if (!file)
            return {};
        file.seekg(entry.archive == 0x7FFF ? dataStart + entry.offset : entry.offset);
        const auto start = data.size();
        data.resize(start + entry.length);
        file.read(data.data() + start, entry.length);
        return file ? data : std::string{};
    }

private:
    std::string base;
    std::uint32_t dataStart = 0;
    std::unordered_map<std::string, VpkEntry> entries;
};

// ---------- меш модели: вершины из .vvd, треугольники из .dx90.vtx, привязка костей из .mdl ----------

struct Vertex {
    float weight[3];
    std::uint8_t bone[3];
    std::uint8_t numBones;
    csgo::Vector position;
    csgo::Vector normal;
    float uv[2];
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct Matrix {
    float m[3][4];
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::vector<Matrix> poseToBone;
    std::vector<std::uint8_t> triangleImage;  // индекс в images для каждого треугольника
    std::vector<Image> images;                // базовые текстуры материалов (может быть пусто)
};

template <typename T>
[[nodiscard]] bool get(const std::string& data, std::size_t offset, T& out) noexcept
{
    if (offset + sizeof(T) > data.size())
        return false;
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}

// ---------- текстуры: .mdl -> имя материала -> .vmt ($basetexture) -> .vtf ----------

[[nodiscard]] std::string normalizePath(std::string path) noexcept
{
    for (auto& c : path)
        c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return path;
}

[[nodiscard]] std::string readCString(const std::string& data, std::size_t offset) noexcept
{
    if (offset >= data.size())
        return {};
    const auto end = data.find('\0', offset);
    return data.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
}

// значение ключа из .vmt (KeyValues): "ключ" "значение" или ключ значение
[[nodiscard]] std::string vmtValue(const std::string& text, std::string_view key) noexcept
{
    for (auto pos = text.find(key); pos != std::string::npos; pos = text.find(key, pos + 1)) {
        auto i = pos + key.size();
        if (i >= text.size() || (text[i] != '"' && !std::isspace(static_cast<unsigned char>(text[i]))))
            continue;  // другой ключ с тем же началом ($basetexture2, $basetexturetransform)
        if (text[i] == '"')
            ++i;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        if (i >= text.size())
            return {};
        if (text[i] == '"') {
            const auto close = text.find('"', i + 1);
            return close == std::string::npos ? std::string{} : text.substr(i + 1, close - i - 1);
        }
        auto end = i;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && text[end] != '}')
            ++end;
        return text.substr(i, end - i);
    }
    return {};
}

[[nodiscard]] std::string baseTextureOf(const Vpk& vpk, const std::string& vmtPath, int depth = 0) noexcept
{
    const auto text = normalizePath(vpk.read(vmtPath));
    if (text.empty())
        return {};
    if (auto base = vmtValue(text, "\"$basetexture"); !base.empty())
        return base;
    if (auto base = vmtValue(text, "$basetexture"); !base.empty())
        return base;
    // patch-материал: берём текстуру из включаемого
    if (depth < 2) {
        if (const auto include = vmtValue(text, "include"); !include.empty())
            return baseTextureOf(vpk, include, depth + 1);
    }
    return {};
}

void decodeColorBlock(const std::uint8_t* block, bool dxt1, std::uint8_t (*out)[4]) noexcept
{
    std::uint16_t c0, c1;
    std::memcpy(&c0, block, 2);
    std::memcpy(&c1, block + 2, 2);
    std::uint8_t palette[4][4];
    const auto expand = [](std::uint16_t c, std::uint8_t* rgb) {
        rgb[0] = static_cast<std::uint8_t>(((c >> 11) & 31) * 255 / 31);
        rgb[1] = static_cast<std::uint8_t>(((c >> 5) & 63) * 255 / 63);
        rgb[2] = static_cast<std::uint8_t>((c & 31) * 255 / 31);
        rgb[3] = 255;
    };
    expand(c0, palette[0]);
    expand(c1, palette[1]);
    for (int k = 0; k < 3; ++k) {
        if (!dxt1 || c0 > c1) {
            palette[2][k] = static_cast<std::uint8_t>((2 * palette[0][k] + palette[1][k]) / 3);
            palette[3][k] = static_cast<std::uint8_t>((palette[0][k] + 2 * palette[1][k]) / 3);
        } else {
            palette[2][k] = static_cast<std::uint8_t>((palette[0][k] + palette[1][k]) / 2);
            palette[3][k] = 0;
        }
    }
    palette[2][3] = palette[3][3] = 255;
    std::uint32_t indices;
    std::memcpy(&indices, block + 4, 4);
    for (int i = 0; i < 16; ++i)
        std::memcpy(out[i], palette[(indices >> (2 * i)) & 3], 4);
}

// мип-уровень не больше maxSize; альфу игнорируем (в базовых текстурах персонажей там маски)
[[nodiscard]] Image decodeVtf(const std::string& data, int maxSize) noexcept
{
    Image image;
    std::uint32_t minor = 0, headerSize = 0;
    std::uint16_t width = 0, height = 0, frames = 1;
    std::int32_t format = -1, lowFormat = -1;
    std::uint8_t mips = 0, lowWidth = 0, lowHeight = 0;
    if (data.size() < 80 || data.compare(0, 3, "VTF") != 0 || !get(data, 8, minor) || !get(data, 12, headerSize) || !get(data, 16, width) || !get(data, 18, height)
        || !get(data, 24, frames) || !get(data, 52, format) || !get(data, 56, mips) || !get(data, 57, lowFormat) || !get(data, 61, lowWidth) || !get(data, 62, lowHeight))
        return image;

    int blockBytes = 0, pixelBytes = 0;
    switch (format) {
    case 13: blockBytes = 8; break;           // DXT1
    case 14: case 15: blockBytes = 16; break; // DXT3/DXT5
    case 0: case 12: pixelBytes = 4; break;   // RGBA8888/BGRA8888
    case 2: case 3: pixelBytes = 3; break;    // RGB888/BGR888
    default: return image;
    }

    std::size_t start = 0;
    if (minor >= 3) {
        std::uint32_t resources = 0;
        get(data, 68, resources);
        for (std::uint32_t i = 0; i < resources && i < 32; ++i) {
            if (80 + 8 * i + 8 <= data.size() && data[80 + 8 * i] == '0' && data[81 + 8 * i] == 0 && data[82 + 8 * i] == 0) {
                std::uint32_t offset = 0;
                get(data, 84 + 8 * i, offset);
                start = offset;
            }
        }
    } else {
        start = headerSize + (lowFormat == -1 ? 0 : std::max(1, (lowWidth + 3) / 4) * std::max(1, (lowHeight + 3) / 4) * 8);
    }
    if (start == 0 || mips == 0)
        return image;

    const auto levelSize = [&](int level) -> std::size_t {
        const int w = std::max(1, width >> level), h = std::max(1, height >> level);
        return blockBytes ? std::size_t(std::max(1, (w + 3) / 4)) * std::max(1, (h + 3) / 4) * blockBytes : std::size_t(w) * h * pixelBytes;
    };
    int level = 0;
    while (level + 1 < mips && (width >> level) > maxSize)
        ++level;
    std::size_t offset = start;
    for (int l = mips - 1; l > level; --l)
        offset += levelSize(l) * std::max<std::uint16_t>(frames, 1);
    if (offset + levelSize(level) > data.size())
        return image;

    const int w = std::max(1, width >> level), h = std::max(1, height >> level);
    image.width = w;
    image.height = h;
    image.rgba.resize(std::size_t(w) * h * 4);
    const auto source = reinterpret_cast<const std::uint8_t*>(data.data()) + offset;

    if (blockBytes) {
        const int blocksX = std::max(1, (w + 3) / 4), blocksY = std::max(1, (h + 3) / 4);
        for (int by = 0; by < blocksY; ++by) {
            for (int bx = 0; bx < blocksX; ++bx) {
                std::uint8_t pixels[16][4];
                decodeColorBlock(source + (std::size_t(by) * blocksX + bx) * blockBytes + (blockBytes - 8), format == 13, pixels);
                for (int i = 0; i < 16; ++i) {
                    const int x = bx * 4 + i % 4, y = by * 4 + i / 4;
                    if (x < w && y < h)
                        std::memcpy(&image.rgba[(std::size_t(y) * w + x) * 4], pixels[i], 4);
                }
            }
        }
    } else {
        for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
            const auto* p = source + i * pixelBytes;
            const bool bgr = format == 12 || format == 3;
            auto* out = &image.rgba[i * 4];
            out[0] = bgr ? p[2] : p[0];
            out[1] = p[1];
            out[2] = bgr ? p[0] : p[2];
            out[3] = 255;
        }
    }
    return image;
}

void loadTextures(const Vpk& vpk, const std::string& mdl, Mesh& mesh) noexcept
{
    std::int32_t numTextures = 0, textureIndex = 0, numCdTextures = 0, cdTextureIndex = 0;
    if (!get(mdl, 204, numTextures) || !get(mdl, 208, textureIndex) || !get(mdl, 212, numCdTextures) || !get(mdl, 216, cdTextureIndex))
        return;
    if (numTextures <= 0 || numTextures > 64 || numCdTextures < 0 || numCdTextures > 64)
        return;

    std::vector<std::string> directories;
    for (int i = 0; i < numCdTextures; ++i) {
        std::int32_t offset = 0;
        if (!get(mdl, cdTextureIndex + 4 * i, offset))
            continue;
        auto directory = normalizePath(readCString(mdl, offset));
        if (!directory.empty() && directory.back() != '/')
            directory += '/';
        directories.push_back(std::move(directory));
    }

    mesh.images.resize(numTextures);
    for (int i = 0; i < numTextures; ++i) {
        std::int32_t nameOffset = 0;
        if (!get(mdl, textureIndex + 64 * i, nameOffset))
            continue;
        const auto name = normalizePath(readCString(mdl, textureIndex + 64 * i + nameOffset));
        for (const auto& directory : directories) {
            const auto base = baseTextureOf(vpk, "materials/" + directory + name + ".vmt");
            if (base.empty())
                continue;
            mesh.images[i] = decodeVtf(vpk.read("materials/" + base + ".vtf"), 256);
            break;
        }
    }
}

[[nodiscard]] std::shared_ptr<Mesh> loadMesh(const Vpk& vpk, const std::string& modelPath, int body) noexcept
{
    const auto stem = modelPath.substr(0, modelPath.size() - 4);  // без «.mdl»
    const auto mdl = vpk.read(modelPath);
    const auto vvd = vpk.read(stem + ".vvd");
    const auto vtx = vpk.read(stem + ".dx90.vtx");
    if (mdl.size() < 240 || vvd.size() < 64 || vtx.size() < 36 || mdl.compare(0, 4, "IDST") != 0 || vvd.compare(0, 4, "IDSV") != 0)
        return nullptr;

    auto mesh = std::make_shared<Mesh>();

    // кости: mstudiobone_t по 216 байт, poseToBone на +0x60
    std::int32_t numBones = 0, boneIndex = 0;
    if (!get(mdl, 156, numBones) || !get(mdl, 160, boneIndex) || numBones <= 0 || numBones > MAXSTUDIOBONES)
        return nullptr;
    mesh->poseToBone.resize(numBones);
    for (int i = 0; i < numBones; ++i) {
        if (!get(mdl, boneIndex + 216 * i + 0x60, mesh->poseToBone[i]))
            return nullptr;
    }

    // вершины LOD0 (с учётом таблицы фиксапов)
    std::int32_t lodVertexes = 0, numFixups = 0, fixupStart = 0, vertexStart = 0;
    if (!get(vvd, 16, lodVertexes) || !get(vvd, 48, numFixups) || !get(vvd, 52, fixupStart) || !get(vvd, 56, vertexStart))
        return nullptr;
    const auto readVertex = [&](std::int32_t index, Vertex& out) {
        const std::size_t offset = vertexStart + static_cast<std::size_t>(index) * 48;
        return get(vvd, offset, out.weight) && get(vvd, offset + 12, out.bone) && get(vvd, offset + 15, out.numBones)
            && get(vvd, offset + 16, out.position) && get(vvd, offset + 28, out.normal) && get(vvd, offset + 40, out.uv);
    };
    if (numFixups > 0) {
        for (int i = 0; i < numFixups; ++i) {
            std::int32_t fixup[3];
            if (!get(vvd, fixupStart + 12 * i, fixup))
                return nullptr;
            if (fixup[0] < 0)
                continue;
            for (int j = 0; j < fixup[2]; ++j) {
                Vertex vertex;
                if (!readVertex(fixup[1] + j, vertex))
                    return nullptr;
                mesh->vertices.push_back(vertex);
            }
        }
    } else {
        mesh->vertices.resize(lodVertexes);
        for (int i = 0; i < lodVertexes; ++i) {
            if (!readVertex(i, mesh->vertices[i]))
                return nullptr;
        }
    }
    for (auto& vertex : mesh->vertices) {
        vertex.numBones = std::min<std::uint8_t>(vertex.numBones, 3);
        for (int k = 0; k < vertex.numBones; ++k) {
            if (vertex.bone[k] >= numBones)
                vertex.numBones = 0;
        }
    }

    std::int32_t numSkinRef = 0, skinIndex = 0;
    get(mdl, 220, numSkinRef);
    get(mdl, 228, skinIndex);

    // треугольники: bodypart -> model (по m_nBody) -> mesh -> strip group, LOD0
    std::int32_t numBodyParts = 0, bodyPartIndex = 0, vtxBodyPartOffset = 0;
    if (!get(mdl, 232, numBodyParts) || !get(mdl, 236, bodyPartIndex) || !get(vtx, 32, vtxBodyPartOffset))
        return nullptr;

    for (int part = 0; part < numBodyParts; ++part) {
        const std::size_t partAddress = bodyPartIndex + 16 * part;
        std::int32_t numModels = 0, partBase = 1, modelIndex = 0;
        if (!get(mdl, partAddress + 4, numModels) || !get(mdl, partAddress + 8, partBase) || !get(mdl, partAddress + 12, modelIndex) || numModels <= 0)
            continue;
        const int model = partBase > 0 ? (body / partBase) % numModels : 0;

        const std::size_t modelAddress = partAddress + modelIndex + 148 * model;
        std::int32_t numMeshes = 0, meshIndex = 0, vertexIndex = 0;
        if (!get(mdl, modelAddress + 72, numMeshes) || !get(mdl, modelAddress + 76, meshIndex) || !get(mdl, modelAddress + 84, vertexIndex))
            continue;

        const std::size_t vtxPart = vtxBodyPartOffset + 8 * part;
        std::int32_t vtxModelOffset = 0;
        if (!get(vtx, vtxPart + 4, vtxModelOffset))
            continue;
        const std::size_t vtxModel = vtxPart + vtxModelOffset + 8 * model;
        std::int32_t lodOffset = 0;
        if (!get(vtx, vtxModel + 4, lodOffset))
            continue;
        const std::size_t vtxLod = vtxModel + lodOffset;
        std::int32_t vtxMeshOffset = 0;
        if (!get(vtx, vtxLod + 4, vtxMeshOffset))
            continue;

        for (int meshNumber = 0; meshNumber < numMeshes; ++meshNumber) {
            std::int32_t meshVertexOffset = 0, material = 0;
            if (!get(mdl, modelAddress + meshIndex + 116 * meshNumber + 12, meshVertexOffset) || !get(mdl, modelAddress + meshIndex + 116 * meshNumber, material))
                continue;
            // материал меша -> текстура через таблицу скинов (семейство 0)
            std::int16_t texture = -1;
            if (material >= 0 && material < numSkinRef)
                get(mdl, skinIndex + 2 * material, texture);
            const std::size_t vtxMesh = vtxLod + vtxMeshOffset + 9 * meshNumber;
            std::int32_t numStripGroups = 0, stripGroupOffset = 0;
            if (!get(vtx, vtxMesh, numStripGroups) || !get(vtx, vtxMesh + 4, stripGroupOffset))
                continue;
            const std::uint32_t base = vertexIndex / 48 + meshVertexOffset;

            for (int group = 0; group < numStripGroups; ++group) {
                // StripGroupHeader_t в CS:GO — 33 байта (есть поля топологии)
                const std::size_t stripGroup = vtxMesh + stripGroupOffset + 33 * group;
                std::int32_t header[4];
                if (!get(vtx, stripGroup, header))
                    continue;
                const auto [numVerts, vertOffset, numIndices, indexOffset] = header;
                for (int i = 0; i + 2 < numIndices; i += 3) {
                    std::array<std::uint32_t, 3> triangle;
                    bool ok = true;
                    for (int k = 0; k < 3; ++k) {
                        std::uint16_t index = 0, original = 0;
                        if (!get(vtx, stripGroup + indexOffset + 2 * (i + k), index) || index >= numVerts
                            || !get(vtx, stripGroup + vertOffset + 9 * index + 4, original)) {
                            ok = false;
                            break;
                        }
                        triangle[k] = base + original;
                        if (triangle[k] >= mesh->vertices.size())
                            ok = false;
                    }
                    if (ok) {
                        mesh->triangles.push_back(triangle);
                        mesh->triangleImage.push_back(texture >= 0 && texture < 255 ? static_cast<std::uint8_t>(texture) : 255);
                    }
                }
            }
        }
    }

    if (mesh->triangles.empty())
        return nullptr;
    loadTextures(vpk, mdl, *mesh);
    return mesh;
}

// ---------- кэш мешей, загрузка в фоне ----------

std::mutex cacheMutex;
std::unordered_map<std::string, std::shared_ptr<Mesh>> meshCache;  // nullptr — не удалось
std::atomic<bool> loading = false;
Vpk vpk;
std::atomic<bool> vpkOpened = false;
std::atomic<bool> vpkFailed = false;

[[nodiscard]] std::shared_ptr<Mesh> requestMesh(const std::string& modelPath, int body) noexcept
{
    const auto key = modelPath + '#' + std::to_string(body);
    {
        std::scoped_lock lock{ cacheMutex };
        if (const auto it = meshCache.find(key); it != meshCache.end())
            return it->second;
    }
    if (vpkFailed || loading.exchange(true))
        return nullptr;

    std::thread{ [modelPath, body, key] {
        if (!vpkOpened) {
            vpkOpened = true;
            vpkFailed = !vpk.open();
        }
        auto mesh = vpkFailed ? nullptr : loadMesh(vpk, modelPath, body);
        {
            std::scoped_lock lock{ cacheMutex };
            meshCache[key] = std::move(mesh);
        }
        loading = false;
    } }.detach();
    return nullptr;
}

// ---------- то, что рисуем: вершины уже в системе модели (x — вперёд, y — влево, z — вверх) ----------

struct Capsule {
    csgo::Vector a, b;
    float radius;
};

struct Snapshot {
    std::shared_ptr<Mesh> mesh;
    std::vector<csgo::Vector> positions;
    std::vector<csgo::Vector> normals;
    std::vector<Capsule> capsules;  // запасной вариант, если меш не загрузился
};

std::mutex snapshotMutex;
Snapshot snapshot;

constexpr const char* defaultModel = "models/player/custom_player/legacy/tm_phoenix.mdl";
std::mutex menuModelMutex;
std::string menuModel = defaultModel;
// когда меню само показывает модель (вкладка агентов), игровой снимок её не перетирает
std::atomic<std::int64_t> menuPinnedUntil = 0;

[[nodiscard]] std::int64_t nowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

[[nodiscard]] Matrix multiply(const float a[3][4], const Matrix& b) noexcept
{
    Matrix r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = a[i][0] * b.m[0][j] + a[i][1] * b.m[1][j] + a[i][2] * b.m[2][j] + (j == 3 ? a[i][3] : 0.0f);
        }
    }
    return r;
}

struct Face {
    float depth;
    std::uint32_t index;
    float brightness;
};

// текстуры загружаются в GL лениво, в потоке отрисовки; держим только для текущего меша
void drawTextured(ImDrawList* drawList, const Mesh& mesh, const std::vector<csgo::Vector>& normals, const std::vector<ImVec2>& screen, const std::vector<Face>& faces, float c, float s, const csgo::Vector& light) noexcept
{
    static const Mesh* uploadedFor = nullptr;
    static auto& textures = *new std::vector<Texture>;  // не разрушаем при выходе: GL-контекста уже может не быть
    if (uploadedFor != &mesh) {
        textures.clear();
        textures.resize(mesh.images.size());
        for (std::size_t i = 0; i < mesh.images.size(); ++i) {
            if (const auto& image = mesh.images[i]; !image.rgba.empty())
                textures[i].init(image.width, image.height, image.rgba.data());
        }
        uploadedFor = &mesh;
    }

    // освещение по вершинам — без «гранёности»
    std::vector<ImU32> vertexColor(normals.size());
    for (std::size_t i = 0; i < normals.size(); ++i) {
        const auto& n = normals[i];
        const float nx = c * n.x - s * n.y, ny = s * n.x + c * n.y, nz = n.z;
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        const float lambert = length > 0.0f ? std::max(0.0f, (nx * light.x + ny * light.y + nz * light.z) / length) : 0.0f;
        const auto value = static_cast<int>(255.0f * std::min(1.0f, 0.45f + 0.7f * lambert));
        vertexColor[i] = IM_COL32(value, value, value, 255);
    }

    const ImTextureID fontTexture = ImGui::GetIO().Fonts->TexID;
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    ImTextureID current = nullptr;
    for (const auto& face : faces) {
        const auto imageIndex = mesh.triangleImage[face.index];
        const ImTextureID texture = imageIndex < textures.size() && textures[imageIndex].get() ? textures[imageIndex].get() : nullptr;
        const ImTextureID wanted = texture ? texture : fontTexture;
        if (wanted != current) {
            if (current)
                drawList->PopTextureID();
            drawList->PushTextureID(wanted);
            current = wanted;
        }
        drawList->PrimReserve(3, 3);
        for (const auto vertex : mesh.triangles[face.index]) {
            const auto& uv = mesh.vertices[vertex].uv;
            drawList->PrimVtx(screen[vertex], texture ? ImVec2{ uv[0], uv[1] } : white, texture ? vertexColor[vertex] : IM_COL32(120, 126, 140, 255));
        }
    }
    if (current)
        drawList->PopTextureID();
}


}

void setMenuModel(const char* path) noexcept
{
    std::scoped_lock lock{ menuModelMutex };
    menuModel = path && *path ? path : defaultModel;
}

void captureMenu(bool pin) noexcept
{
    if (pin)
        menuPinnedUntil = nowMs() + 150;
    std::string modelPath;
    {
        std::scoped_lock lock{ menuModelMutex };
        modelPath = menuModel;
    }
    Snapshot result;
    {
        // главное меню: выбранный агент (или модель по умолчанию) в базовой позе
        if (auto mesh = requestMesh(modelPath, 0)) {
            result.positions.reserve(mesh->vertices.size());
            for (const auto& vertex : mesh->vertices) {
                // в позе привязки: y — вверх, z — вперёд, x — вбок
                result.positions.push_back({ vertex.position.z, vertex.position.x, vertex.position.y });
                result.normals.push_back({ vertex.normal.z, vertex.normal.x, vertex.normal.y });
            }
            result.mesh = std::move(mesh);
        }
        std::scoped_lock lock{ snapshotMutex };
        snapshot = std::move(result);
    }
}

void capture(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory) noexcept
{
    Snapshot result;
    if (!engineInterfaces.getEngine().isInGame() || nowMs() < menuPinnedUntil)
        return;

    // своя модель, а если мёртв — первый живой игрок
    csgo::Entity entity = localPlayer ? localPlayer.get() : csgo::Entity::from(retSpoofGadgets->client, nullptr);
    if (!entity.getPOD() || !entity.isAlive()) {
        entity = csgo::Entity::from(retSpoofGadgets->client, nullptr);
        for (int i = 1; i <= engineInterfaces.getEngine().getMaxClients(); ++i) {
            const auto other = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
            if (other.getPOD() && !other.getNetworkable().isDormant() && other.isAlive()) {
                entity = other;
                break;
            }
        }
    }
    if (!entity.getPOD())
        return;

    const auto model = entity.getRenderable().getModel();
    if (!model)
        return;
    const auto studio = engineInterfaces.getModelInfo().getStudioModel(model);
    if (!studio)
        return;

    static csgo::matrix3x4 bones[MAXSTUDIOBONES];
    // все кости, не только хитбоксы — меш висит и на пальцах, и на одежде
    if (!entity.setupBones(bones, MAXSTUDIOBONES, 0x7FF00, memory.globalVars->currenttime))
        return;

    const auto origin = entity.getAbsOrigin();
    const float yaw = Helpers::deg2rad(entity.getAbsAngle().y);
    const float c = std::cos(-yaw), s = std::sin(-yaw);
    const auto toModel = [&](const csgo::Vector& world) {
        const float x = world.x - origin.x, y = world.y - origin.y;
        return csgo::Vector{ c * x - s * y, s * x + c * y, world.z - origin.z };
    };
    const auto rotateOnly = [&](const csgo::Vector& v) {
        return csgo::Vector{ c * v.x - s * v.y, s * v.x + c * v.y, v.z };
    };

    if (auto mesh = requestMesh(model->name, entity.body())) {
        // скиннинг: мир = сумма весов * (кость->мир * поза->кость) * вершина
        const int numBones = static_cast<int>(mesh->poseToBone.size());
        static Matrix skin[MAXSTUDIOBONES];
        for (int i = 0; i < numBones; ++i) {
            float boneToWorld[3][4];
            std::memcpy(boneToWorld, &bones[i], sizeof(boneToWorld));
            skin[i] = multiply(boneToWorld, mesh->poseToBone[i]);
        }

        result.positions.resize(mesh->vertices.size());
        result.normals.resize(mesh->vertices.size());
        for (std::size_t v = 0; v < mesh->vertices.size(); ++v) {
            const auto& vertex = mesh->vertices[v];
            csgo::Vector position{}, normal{};
            for (int k = 0; k < vertex.numBones; ++k) {
                const auto& m = skin[vertex.bone[k]].m;
                const float w = vertex.weight[k];
                const auto& p = vertex.position;
                const auto& n = vertex.normal;
                position.x += w * (m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3]);
                position.y += w * (m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3]);
                position.z += w * (m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]);
                normal.x += w * (m[0][0] * n.x + m[0][1] * n.y + m[0][2] * n.z);
                normal.y += w * (m[1][0] * n.x + m[1][1] * n.y + m[1][2] * n.z);
                normal.z += w * (m[2][0] * n.x + m[2][1] * n.y + m[2][2] * n.z);
            }
            result.positions[v] = toModel(position);
            result.normals[v] = rotateOnly(normal);
        }
        result.mesh = std::move(mesh);
    } else if (const auto set = studio->getHitboxSet(entity.hitboxSet())) {
        // меш ещё грузится — пока капсулы хитбоксов
        for (int i = 0; i < set->numHitboxes; ++i) {
            const auto hitbox = set->getHitbox(i);
            if (!hitbox || hitbox->bone < 0 || hitbox->bone >= MAXSTUDIOBONES)
                continue;
            const auto& bone = bones[hitbox->bone];
            result.capsules.push_back({ toModel(transform(hitbox->bbMin, bone)), toModel(transform(hitbox->bbMax, bone)), hitbox->capsuleRadius > 0.0f ? hitbox->capsuleRadius : 3.0f });
        }
    }

    std::scoped_lock lock{ snapshotMutex };
    snapshot = std::move(result);
}

bool draw(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, ImU32 visible, ImU32 hidden, ImVec2& boundsMin, ImVec2& boundsMax, bool textured) noexcept
{
    Snapshot model;
    {
        std::scoped_lock lock{ snapshotMutex };
        model = snapshot;
    }
    if (!model.mesh && model.capsules.empty())
        return false;

    // стоит лицом к нам; повернуть — потянуть мышью
    static float spin = 0.0f;
    if (ImGui::IsMouseHoveringRect(min, max) && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        spin -= ImGui::GetIO().MouseDelta.x * 0.012f;
    const float c = std::cos(spin), s = std::sin(spin);

    // высота модели для масштаба
    float top = -1e9f, bottom = 1e9f;
    if (model.mesh) {
        for (const auto& p : model.positions) {
            top = std::max(top, p.z);
            bottom = std::min(bottom, p.z);
        }
    } else {
        for (const auto& capsule : model.capsules) {
            top = std::max({ top, capsule.a.z + capsule.radius, capsule.b.z + capsule.radius });
            bottom = std::min({ bottom, capsule.a.z - capsule.radius, capsule.b.z - capsule.radius });
        }
    }
    const float height = std::max(top - bottom, 1.0f);
    const float scale = (max.y - min.y) * 0.9f / height;
    const ImVec2 center{ (min.x + max.x) * 0.5f, max.y - (max.y - min.y) * 0.05f };

    // зритель стоит на +x и смотрит на модель; depth — насколько точка ближе к нему
    const auto project = [&](const csgo::Vector& p, float& depth) {
        const float x = c * p.x - s * p.y;
        const float y = s * p.x + c * p.y;
        depth = x;
        return ImVec2{ center.x - y * scale, center.y - (p.z - bottom) * scale };
    };

    const ImU32 base = visible ? visible : hidden ? hidden : IM_COL32(150, 158, 178, 255);
    const auto baseColor = ImGui::ColorConvertU32ToFloat4(base);
    const auto shade = [&baseColor](float light) {
        return ImGui::ColorConvertFloat4ToU32({ baseColor.x * light, baseColor.y * light, baseColor.z * light, baseColor.w });
    };

    boundsMin = { 1e9f, 1e9f };
    boundsMax = { -1e9f, -1e9f };

    if (model.mesh) {
        const auto& positions = model.positions;
        std::vector<ImVec2> screen(positions.size());
        std::vector<float> depth(positions.size());
        for (std::size_t i = 0; i < positions.size(); ++i) {
            screen[i] = project(positions[i], depth[i]);
            boundsMin = { std::min(boundsMin.x, screen[i].x), std::min(boundsMin.y, screen[i].y) };
            boundsMax = { std::max(boundsMax.x, screen[i].x), std::max(boundsMax.y, screen[i].y) };
        }

        // свет спереди-сверху-слева относительно зрителя
        const csgo::Vector light{ 0.75f, 0.35f, 0.55f };
        std::vector<Face> faces;
        faces.reserve(model.mesh->triangles.size());
        for (std::uint32_t t = 0; t < model.mesh->triangles.size(); ++t) {
            const auto& [a, b, d] = model.mesh->triangles[t];
            // нормаль треугольника по нормалям вершин, повёрнутая вместе с моделью
            const auto& na = model.normals[a];
            const auto& nb = model.normals[b];
            const auto& nd = model.normals[d];
            const float nx0 = na.x + nb.x + nd.x, ny0 = na.y + nb.y + nd.y, nz = na.z + nb.z + nd.z;
            const float nx = c * nx0 - s * ny0, ny = s * nx0 + c * ny0;
            const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (length <= 0.0f || nx / length < -0.15f)
                continue;  // смотрит от зрителя
            const float lambert = std::max(0.0f, (nx * light.x + ny * light.y + nz * light.z) / length);
            faces.push_back({ depth[a] + depth[b] + depth[d], t, 0.32f + 0.68f * lambert });
        }
        // дальние первыми
        std::ranges::sort(faces, {}, &Face::depth);

        // без сглаживания заливки, иначе между треугольниками видны швы
        const auto flags = drawList->Flags;
        drawList->Flags &= ~ImDrawListFlags_AntiAliasedFill;

        // «за стеной» — силуэт вторым цветом чуть шире, под основной моделью
        if (visible && hidden) {
            for (const auto& face : faces) {
                const auto& [a, b, d] = model.mesh->triangles[face.index];
                const ImVec2 offset{ 1.5f, 0.0f };
                drawList->AddTriangleFilled({ screen[a].x + offset.x, screen[a].y }, { screen[b].x + offset.x, screen[b].y }, { screen[d].x + offset.x, screen[d].y }, hidden);
            }
        }
        if (textured && !model.mesh->images.empty()) {
            drawTextured(drawList, *model.mesh, model.normals, screen, faces, c, s, light);
        } else {
            for (const auto& face : faces) {
                const auto& [a, b, d] = model.mesh->triangles[face.index];
                drawList->AddTriangleFilled(screen[a], screen[b], screen[d], shade(face.brightness));
            }
        }
        drawList->Flags = flags;
        return true;
    }

    // запасной вариант: капсулы хитбоксов
    struct Projected {
        ImVec2 a, b;
        float radius;
        float depth;
    };
    std::vector<Projected> projected;
    for (const auto& capsule : model.capsules) {
        float da, db;
        const auto a = project(capsule.a, da);
        const auto b = project(capsule.b, db);
        const float r = capsule.radius * scale;
        projected.push_back({ a, b, r, (da + db) * 0.5f });
        boundsMin = { std::min({ boundsMin.x, a.x - r, b.x - r }), std::min({ boundsMin.y, a.y - r, b.y - r }) };
        boundsMax = { std::max({ boundsMax.x, a.x + r, b.x + r }), std::max({ boundsMax.y, a.y + r, b.y + r }) };
    }
    std::ranges::sort(projected, {}, &Projected::depth);
    float minDepth = 1e9f, maxDepth = -1e9f;
    for (const auto& p : projected) {
        minDepth = std::min(minDepth, p.depth);
        maxDepth = std::max(maxDepth, p.depth);
    }
    for (const auto& p : projected) {
        const float t = maxDepth > minDepth ? (p.depth - minDepth) / (maxDepth - minDepth) : 1.0f;
        const auto color = shade(0.55f + 0.45f * t);
        drawList->AddLine(p.a, p.b, color, p.radius * 2.0f);
        drawList->AddCircleFilled(p.a, p.radius, color, 16);
        drawList->AddCircleFilled(p.b, p.radius, color, 16);
    }
    return true;
}

}
