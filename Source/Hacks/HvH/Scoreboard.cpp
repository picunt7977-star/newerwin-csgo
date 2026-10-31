#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <link.h>

#include "HvhConfig.h"
#include "Scoreboard.h"

#include <Interfaces/ClientInterfaces.h>
#include <Interfaces/EngineInterfaces.h>
#include <Interfaces/OtherInterfaces.h>
#include <Memory.h>
#include <CSGO/Engine.h>
#include <CSGO/Entity.h>
#include <CSGO/EntityList.h>
#include <CSGO/LocalPlayer.h>
#include <CSGO/Panorama.h>
#include <CSGO/WeaponData.h>
#include <CSGO/WeaponId.h>
#include <RetSpoof/RetSpoofGadgets.h>

// Снаряжение врагов прямо в строках таблицы (Tab). Таблица — Panorama (scoreboard.js): строка игрока —
// панель с id "player-<xuid>", ник растягивается на всё свободное место. Вставляем после ячейки ника
// панель с иконками file://{images}/icons/equipment/*.svg — ник ужимается, иконки встают в пустую полосу.
// JS запускаем через CUIEngine::RunScript на панели HUD (её Osiris уже достаёт для killfeed).
namespace hvh::scoreboard
{

namespace
{

// CUIEngine::RunScript(panel, js, schemaPath, int, int, bool, bool) в panorama_client.so (csgo legacy, Linux):
// функция по строке "CUIEngine::RunScript (compile+run)". В таблице CUIEngine она стоит на 111 или 114
// (два vtable) — проверяем, что у объекта в vtable рядом есть именно она, плюс пролог. Не сошлось — выключено.
constexpr std::uintptr_t runScriptOffset = 0xbaba0;
constexpr std::uint8_t runScriptPrologue[]{ 0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x45, 0x89, 0xCF };
using RunScript = void (*)(void* engine, void* panel, const char* script, const char* schemaPath, int, int, bool, bool);

std::uintptr_t panoramaBase = 0;

int findPanorama(dl_phdr_info* info, std::size_t, void*)
{
    if (info->dlpi_name && std::string_view{ info->dlpi_name }.ends_with("/panorama_client.so")) {
        panoramaBase = info->dlpi_addr;
        return 1;
    }
    return 0;
}

[[nodiscard]] RunScript resolveRunScript(void* uiEngine) noexcept
{
    static bool resolved = false;
    static RunScript function = nullptr;
    if (resolved)
        return function;
    resolved = true;
    if (!uiEngine)
        return nullptr;
    dl_iterate_phdr(findPanorama, nullptr);
    if (!panoramaBase)
        return nullptr;
    const auto target = panoramaBase + runScriptOffset;
    if (std::memcmp(reinterpret_cast<const void*>(target), runScriptPrologue, sizeof(runScriptPrologue)) != 0)
        return nullptr;
    const auto vtable = *reinterpret_cast<std::uintptr_t* const*>(uiEngine);
    // по файлу: слот 111 в основном vtable и 114 во втором; ищем в окрестности, чтобы не промахнуться на ±1
    for (int index = 105; index <= 120; ++index) {
        if (vtable[index] == target) {
            function = reinterpret_cast<RunScript>(target);
            break;
        }
    }
    return function;
}

[[nodiscard]] const char* iconName(WeaponId id) noexcept
{
    switch (id) {
    case WeaponId::Deagle: return "deagle";
    case WeaponId::Elite: return "elite";
    case WeaponId::Fiveseven: return "fiveseven";
    case WeaponId::Glock: return "glock";
    case WeaponId::Ak47: return "ak47";
    case WeaponId::Aug: return "aug";
    case WeaponId::Awp: return "awp";
    case WeaponId::Famas: return "famas";
    case WeaponId::G3SG1: return "g3sg1";
    case WeaponId::GalilAr: return "galilar";
    case WeaponId::M249: return "m249";
    case WeaponId::M4A1: return "m4a1";
    case WeaponId::Mac10: return "mac10";
    case WeaponId::P90: return "p90";
    case WeaponId::Mp5sd: return "mp5sd";
    case WeaponId::Ump45: return "ump45";
    case WeaponId::Xm1014: return "xm1014";
    case WeaponId::Bizon: return "bizon";
    case WeaponId::Mag7: return "mag7";
    case WeaponId::Negev: return "negev";
    case WeaponId::Sawedoff: return "sawedoff";
    case WeaponId::Tec9: return "tec9";
    case WeaponId::Taser: return "taser";
    case WeaponId::Hkp2000: return "hkp2000";
    case WeaponId::Mp7: return "mp7";
    case WeaponId::Mp9: return "mp9";
    case WeaponId::Nova: return "nova";
    case WeaponId::P250: return "p250";
    case WeaponId::Scar20: return "scar20";
    case WeaponId::Sg553: return "sg556";
    case WeaponId::Ssg08: return "ssg08";
    case WeaponId::Flashbang: return "flashbang";
    case WeaponId::HeGrenade: return "hegrenade";
    case WeaponId::SmokeGrenade: return "smokegrenade";
    case WeaponId::Molotov: return "molotov";
    case WeaponId::Decoy: return "decoy";
    case WeaponId::IncGrenade: return "incgrenade";
    case WeaponId::C4: return "c4";
    case WeaponId::Healthshot: return "healthshot";
    case WeaponId::M4a1_s: return "m4a1_silencer";
    case WeaponId::Usp_s: return "usp_silencer";
    case WeaponId::Cz75a: return "cz75a";
    case WeaponId::Revolver: return "revolver";
    case WeaponId::TaGrenade: return "tagrenade";
    default: return nullptr;
    }
}

// порядок как в магазине: основное, пистолет, зевс, гранаты, бомба
[[nodiscard]] int iconOrder(WeaponType type) noexcept
{
    switch (type) {
    case WeaponType::SniperRifle: case WeaponType::Rifle: case WeaponType::SubMachinegun:
    case WeaponType::Shotgun: case WeaponType::Machinegun: return 0;
    case WeaponType::Pistol: return 1;
    case WeaponType::Grenade: return 3;
    case WeaponType::C4: return 4;
    default: return 2;
    }
}

// последнее известное снаряжение по индексу: у дорманта сервер оружие не шлёт
std::array<std::string, 65> lastIcons{};

}

void update(const Memory& memory, const OtherInterfaces& interfaces, const ClientInterfaces& clientInterfaces,
    const EngineInterfaces& engineInterfaces, bool open, float realtime) noexcept
{
    static float nextRun = 0.0f;
    static bool wasOpen = false;
    if (!config.view.tabLoadout || !open || !localPlayer) {
        wasOpen = false;
        return;
    }
    // только что открыли — сразу, дальше 4 раза в секунду (иконки меняются, когда враг подбирает/кидает)
    if (wasOpen && realtime < nextRun)
        return;
    wasOpen = true;
    nextRun = realtime + 0.25f;

    const auto uiEngine = interfaces.getPanoramaUIEngine().accessUIEngine();
    const auto runScript = resolveRunScript(uiEngine);
    if (!runScript || !memory.findHudElement || !memory.hud)
        return;
    const auto deathNotice = std::uintptr_t(memory.findHudElement(memory.hud, "CCSGO_HudDeathNotice"));
    if (!deathNotice)
        return;
    // та же цепочка, что в Misc::preserveKillfeed
    const auto hudPanel = *reinterpret_cast<void**>(*reinterpret_cast<std::uintptr_t*>(deathNotice + (-32 + 128)) + sizeof(std::uintptr_t));
    if (!hudPanel)
        return;

    std::string data = "{";
    const int maxClients = std::min(engineInterfaces.getEngine().getMaxClients(), 64);
    for (int i = 1; i <= maxClients; ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (!entity.getPOD() || entity.getPOD() == localPlayer.get().getPOD() || !entity.isPlayer() || !entity.isOtherEnemy(memory, localPlayer.get()))
            continue;
        if (!entity.getNetworkable().isDormant()) {
            std::string icons;
            if (entity.isAlive()) {
                std::array<std::vector<const char*>, 5> groups;
                for (const int handle : entity.weapons()) {
                    if (handle == -1 || handle == 0)
                        continue;
                    const auto weapon = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntityFromHandle(handle));
                    if (!weapon.getPOD() || weapon.getWeaponType() == WeaponType::Knife)
                        continue;
                    if (const auto name = iconName(weapon.itemDefinitionIndex()))
                        groups[iconOrder(weapon.getWeaponType())].push_back(name);
                }
                if (entity.armor() > 0)
                    groups[2].insert(groups[2].begin(), entity.hasHelmet() ? "armor_helmet" : "armor");
                if (entity.hasDefuser())
                    groups[4].push_back("defuser");
                for (const auto& group : groups) {
                    for (const auto name : group) {
                        icons += icons.empty() ? "\"" : ",\"";
                        icons += name;
                        icons += '"';
                    }
                }
            }
            lastIcons[i] = icons;
        }
        if (data.size() > 1)
            data += ',';
        data += '"' + std::to_string(i) + "\":[" + lastIcons[i] + ']';
    }
    data += '}';

    // Строка: Panel "player-<xuid>" > Panel.sb-row (flow right) > Frame > Panel.sb-row__cell--name (flow left:
    // ник Label и значки справа налево). Иконки кладём внутрь ячейки ника первым ребёнком — они встают у её
    // правого края, перед Money, а ник ужимается (width fill-parent-flow вместо 100%).
    const std::string script = "(function(){var d=" + data + R"js(;
var p=$.GetContextPanel();while(p.GetParent())p=p.GetParent();
var sb=p.FindChildTraverse("Scoreboard");if(!sb)return;
for(var i in d){
 var x=GameStateAPI.GetPlayerXuidStringFromEntIndex(parseInt(i));if(!x||x=="0")continue;
 var row=sb.FindChildTraverse("player-"+x);if(!row||!row.IsValid())continue;
 var cells=row.FindChildrenWithClassTraverse("sb-row__cell--name");if(!cells.length)continue;
 var cell=cells[0];
 var box=cell.FindChildTraverse("newerwin-gear");
 if(!box){
  box=$.CreatePanel("Panel",cell,"newerwin-gear");
  box.style.flowChildren="right";box.style.height="100%";box.style.verticalAlign="center";
  box.style.marginLeft="6px";box.style.marginRight="4px";
  var first=cell.GetChild(0);if(first&&first!=box)cell.MoveChildBefore(box,first);
  var nm=cell.FindChildTraverse("name");if(nm)nm.style.width="fill-parent-flow(1.0)";
 }
 var key=d[i].join(",");if(box.newerwinKey===key)continue;box.newerwinKey=key;
 box.RemoveAndDeleteChildren();
 d[i].forEach(function(w){
  var im=$.CreatePanel("Image",box,"");
  im.SetImage("file://{images}/icons/equipment/"+w+".svg");
  im.style.height="18px";im.style.verticalAlign="center";im.style.marginRight="5px";
  im.style.washColor=w=="c4"?"#ff6a6a":(w=="defuser"?"#6ab4ff":"#e8e8e8");
  try{im.SetScaling("stretch-to-fit-y-preserve-aspect");}catch(e){}
 });
}})();)js";
    runScript(uiEngine, hudPanel, script.c_str(), "panorama/layout/base.xml", 8, 10, false, false);
}

}
