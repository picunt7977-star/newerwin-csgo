#pragma once

#include <array>
#include <cassert>

#include "ChamsMaterial.h"
#include "ChamsMaterialFactory.h"
#include <CSGO/Material.h>

class ChamsMaterials {
public:
    explicit ChamsMaterials(const ChamsMaterialFactory& factory)
        : materialFactory{ factory }
    {
    }

    static constexpr auto numberOfMaterials = 13;

    // Рендер в CS:GO отложенный: цвет и флаги материала читаются, когда кадр уже собран.
    // Поэтому у прохода «сквозь стены» и «видимого» должны быть разные материалы,
    // иначе оба рисуются с последними выставленными цветом и IGNOREZ.
    [[nodiscard]] csgo::Material getMaterial(ChamsMaterial material, bool ignorez = false)
    {
        const auto materialIndex = static_cast<std::size_t>(material);
        assert(materialIndex < numberOfMaterials);

        auto& slot = ignorez ? xqzMaterials : materials;
        auto& initialized = ignorez ? xqzInitialized : materialsInitialized;
        if (!initialized[materialIndex]) {
            slot[materialIndex] = materialFactory(material, ignorez);
            initialized[materialIndex] = true;
        }
        return csgo::Material::from(retSpoofGadgets->client, slot[materialIndex]);
    }

private:
    ChamsMaterialFactory materialFactory;
    std::array<csgo::MaterialPOD*, numberOfMaterials> materials{};
    std::array<bool, numberOfMaterials> materialsInitialized{};
    std::array<csgo::MaterialPOD*, numberOfMaterials> xqzMaterials{};
    std::array<bool, numberOfMaterials> xqzInitialized{};
};
