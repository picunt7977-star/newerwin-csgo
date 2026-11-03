#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <array>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <imgui/imgui.h>
#include <link.h>

#include "HvhConfig.h"
#include "HvhGui.h"
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

// Снаряжение врагов по Tab — иконками прямо в строке таблицы, посередине пустой полосы между ником и
// деньгами. Таблица — Panorama: JS через CUIEngine::RunScript. Раньше блок с иконками стоял в потоке
// ячейки ника и ужимал её — иконки наезжали на ник и деньги. Теперь блок нулевой ширины (раскладку не
// трогает), иконки рисуются за его пределами и сдвигаются transform-ом в середину свободного места.
// Если RunScript не нашёлся — запасной вариант: своя панель справа от таблицы (ImGui).
namespace hvh::scoreboard
{

namespace
{

// CUIEngine::RunScript(panel, js, schemaPath, int, int, bool, bool) (csgo legacy, Linux): функция, которая
// ссылается на строку "CUIEngine::RunScript (compile+run)". Игра грузит panorama_gl_client.so (из неё же
// Osiris берёт PanoramaUIEngine), а смещение раньше было только для panorama_client.so — поэтому иконки
// в Tab не появлялись никогда и всегда срабатывала запасная панель. Проверяем пролог и то, что функция
// стоит в vtable движка (слоты 105–120). Не сошлось — выключено, работает запасная панель.
struct RunScriptLocation {
    const char* library;
    std::uintptr_t offset;
};
constexpr RunScriptLocation runScriptLocations[]{
    { "/panorama_gl_client.so", 0xbb6c0 },
    { "/panorama_client.so", 0xbaba0 },
};
constexpr std::uint8_t runScriptPrologue[]{ 0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x45, 0x89, 0xCF };
using RunScript = void (*)(void* engine, void* panel, const char* script, const char* schemaPath, int, int, bool, bool);

std::uintptr_t panoramaBase = 0;
std::uintptr_t runScriptOffset = 0;

int findPanorama(dl_phdr_info* info, std::size_t, void*)
{
    if (!info->dlpi_name)
        return 0;
    for (const auto& location : runScriptLocations) {
        if (std::string_view{ info->dlpi_name }.ends_with(location.library)) {
            panoramaBase = info->dlpi_addr;
            runScriptOffset = location.offset;
            return 1;
        }
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

[[nodiscard]] const char* shortName(WeaponId id) noexcept
{
    switch (id) {
    case WeaponId::Deagle: return "Deagle";
    case WeaponId::Elite: return "Dualies";
    case WeaponId::Fiveseven: return "Five-SeveN";
    case WeaponId::Glock: return "Glock";
    case WeaponId::Ak47: return "AK-47";
    case WeaponId::Aug: return "AUG";
    case WeaponId::Awp: return "AWP";
    case WeaponId::Famas: return "FAMAS";
    case WeaponId::G3SG1: return "G3SG1";
    case WeaponId::GalilAr: return "Galil";
    case WeaponId::M249: return "M249";
    case WeaponId::M4A1: return "M4A4";
    case WeaponId::Mac10: return "MAC-10";
    case WeaponId::P90: return "P90";
    case WeaponId::Mp5sd: return "MP5";
    case WeaponId::Ump45: return "UMP";
    case WeaponId::Xm1014: return "XM1014";
    case WeaponId::Bizon: return "Bizon";
    case WeaponId::Mag7: return "MAG-7";
    case WeaponId::Negev: return "Negev";
    case WeaponId::Sawedoff: return "Sawed-Off";
    case WeaponId::Tec9: return "Tec-9";
    case WeaponId::Taser: return "Zeus";
    case WeaponId::Hkp2000: return "P2000";
    case WeaponId::Mp7: return "MP7";
    case WeaponId::Mp9: return "MP9";
    case WeaponId::Nova: return "Nova";
    case WeaponId::P250: return "P250";
    case WeaponId::Scar20: return "SCAR-20";
    case WeaponId::Sg553: return "SG 553";
    case WeaponId::Ssg08: return "Scout";
    case WeaponId::Flashbang: return "Flash";
    case WeaponId::HeGrenade: return "HE";
    case WeaponId::SmokeGrenade: return "Smoke";
    case WeaponId::Molotov: return "Molotov";
    case WeaponId::Decoy: return "Decoy";
    case WeaponId::IncGrenade: return "Incendiary";
    case WeaponId::C4: return "C4";
    case WeaponId::Healthshot: return "Healthshot";
    case WeaponId::M4a1_s: return "M4A1-S";
    case WeaponId::Usp_s: return "USP-S";
    case WeaponId::Cz75a: return "CZ75";
    case WeaponId::Revolver: return "R8";
    case WeaponId::TaGrenade: return "TA";
    default: return nullptr;
    }
}

// порядок как в магазине: основное, пистолет, зевс и прочее, гранаты, бомба
[[nodiscard]] int order(WeaponType type) noexcept
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

struct Entry {
    std::string name;
    std::string primary;    // основное оружие (или пистолет, если основного нет) — крупно
    std::string rest;       // остальное через « · »
    std::string icons;      // для Panorama: "ak47","deagle",... (имена svg из icons/equipment)
    int health = 0;
    int armor = 0;
    int money = 0;
    bool helmet = false;
    bool kit = false;
    bool bomb = false;
    bool alive = false;
    bool dormant = false;
};

std::mutex mutex;
std::vector<Entry> entries;     // под mutex: пишет игровой поток, читает рендер
bool visible = false;
std::array<Entry, 65> lastKnown{};   // у дорманта сервер оружие не шлёт — держим последнее

}

namespace
{

// true — иконки отданы в таблицу (Panorama), своя панель не нужна
bool injectIcons(const Memory& memory, const OtherInterfaces& interfaces, const std::vector<Entry>& list, const std::vector<int>& indices) noexcept
{
    const auto uiEngine = interfaces.getPanoramaUIEngine().accessUIEngine();
    const auto runScript = resolveRunScript(uiEngine);
    if (!runScript || !memory.findHudElement || !memory.hud)
        return false;
    const auto deathNotice = std::uintptr_t(memory.findHudElement(memory.hud, "CCSGO_HudDeathNotice"));
    if (!deathNotice)
        return false;
    // та же цепочка, что в Misc::preserveKillfeed
    const auto hudPanel = *reinterpret_cast<void**>(*reinterpret_cast<std::uintptr_t*>(deathNotice + (-32 + 128)) + sizeof(std::uintptr_t));
    if (!hudPanel)
        return false;

    std::string data = "{";
    for (std::size_t k = 0; k < list.size(); ++k) {
        if (data.size() > 1)
            data += ',';
        data += '"' + std::to_string(indices[k]) + "\":[" + (list[k].alive ? list[k].icons : std::string{}) + ']';
    }
    data += '}';

    // Строка: Panel "player-<xuid>" > ... > ячейка ника .sb-row__cell--name (до колонки денег).
    // Блок newerwin-gear — первый ребёнок ячейки, ширина 0: ник и остальные значки стоят как стояли.
    // Иконки идут от него влево (flow left, overflow noclip), а translate3d ставит их правый край так,
    // чтобы ряд оказался посередине между концом ника и правым краем ячейки (началом денег).
    // Размеры Panorama отдаёт в пикселях экрана — делим на actualuiscale_x, стили задаются в «px» раскладки.
    const std::string script = "(function(){var d=" + data + R"js(;
var p=$.GetContextPanel();while(p.GetParent())p=p.GetParent();
var once=function(m){if(p.GetAttributeString("nwdbg","")!==m){p.SetAttributeString("nwdbg",m);$.Msg("[newerwin] tab: "+m+"\n");}};
var sc=p.FindChildTraverse("ScoreboardContainer");
var sb=sc?sc.FindChildTraverse("Scoreboard"):null;if(!sb||!sb.IsValid()){once("нет таблицы HUD");return;}
var placed=0,total=0;
for(var i in d){total++;
 var x=GameStateAPI.GetPlayerXuidStringFromEntIndex(parseInt(i));if(!x)continue;
 var row=sb.FindChildTraverse("player-"+x);if(!row||!row.IsValid())continue;
 var cells=row.FindChildrenWithClassTraverse("sb-row__cell--name");if(!cells.length)continue;
 var cell=cells[0];
 var box=cell.FindChildTraverse("newerwin-gear");
 if(!box){
  box=$.CreatePanel("Panel",cell,"newerwin-gear");
  box.style.width="0px";box.style.height="100%";box.style.overflow="noclip";
  box.style.flowChildren="left";box.style.verticalAlign="center";box.hittest=false;
  var first=cell.GetChild(0);if(first&&first!=box)cell.MoveChildBefore(box,first);
 }
 var key=d[i].join(",");
 if(box.GetAttributeString("nwk","-")!==key){
  box.SetAttributeString("nwk",key);
  box.RemoveAndDeleteChildren();
  d[i].forEach(function(w){
   var im=$.CreatePanel("Image",box,"");
   im.SetImage("file://{images}/icons/equipment/"+w+".svg");
   im.style.height="16px";im.style.verticalAlign="center";im.style.marginLeft="5px";
   im.style.washColor=w=="c4"?"#ff6a6a":(w=="defuser"?"#6ab4ff":"#e8e8e8");
   try{im.SetScaling("stretch-to-fit-y-preserve-aspect");}catch(e){}
  });
 }
 var s=cell.actualuiscale_x||1;
 var cw=cell.actuallayoutwidth/s;if(!(cw>0))continue;
 var nm=cell.FindChildTraverse("name");
 var end=0;
 if(nm&&nm.IsValid()){
  var tw=0;try{tw=nm.contentwidth/s;}catch(e){}
  if(!(tw>0)||tw>=cw)tw=((nm.text||"").length)*8;
  end=nm.actualxoffset/s+tw;
 }
 var iw=0;for(var k=0;k<box.GetChildCount();k++){var c=box.GetChild(k);iw+=(c.actuallayoutwidth/s||18)+5;}
 var right=end+(cw-end+iw)/2;
 right=Math.max(end+iw+8,Math.min(right,cw-8));
 var t=right-box.actualxoffset/s;
 box.style.transform="translate3d("+Math.round(t)+"px,0px,0px)";
 placed++;
}
once(placed>0?"ok":"строки не найдены ("+total+")");
})();)js";
    runScript(uiEngine, hudPanel, script.c_str(), "panorama/layout/base.xml", 8, 10, false, false);
    return true;
}

}

void update(const Memory& memory, const OtherInterfaces& interfaces, const ClientInterfaces& clientInterfaces,
    const EngineInterfaces& engineInterfaces, bool open, float realtime) noexcept
{
    static float nextRun = 0.0f;
    static bool wasOpen = false;
    const bool show = config.view.tabLoadout && open && localPlayer;
    if (!show) {
        wasOpen = false;
        std::scoped_lock lock{ mutex };
        visible = false;
        return;
    }
    // только что открыли — сразу, дальше 4 раза в секунду
    if (wasOpen && realtime < nextRun)
        return;
    wasOpen = true;
    nextRun = realtime + 0.25f;

    std::vector<Entry> fresh;
    std::vector<int> indices;
    const int maxClients = std::min(engineInterfaces.getEngine().getMaxClients(), 64);
    for (int i = 1; i <= maxClients; ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (!entity.getPOD() || entity.getPOD() == localPlayer.get().getPOD() || !entity.isPlayer() || !entity.isOtherEnemy(memory, localPlayer.get()))
            continue;
        auto& known = lastKnown[i];
        csgo::PlayerInfo info{};
        if (engineInterfaces.getEngine().getPlayerInfo(i, info))
            known.name = info.name;
        known.dormant = entity.getNetworkable().isDormant();
        if (!known.dormant) {
            known.alive = entity.isAlive();
            known.health = known.alive ? entity.health() : 0;
            known.armor = entity.armor();
            known.helmet = entity.hasHelmet();
            known.kit = entity.hasDefuser();
            known.money = entity.account();
            known.bomb = false;
            std::array<std::vector<const char*>, 5> groups;
            std::array<std::vector<const char*>, 5> iconGroups;
            if (known.alive) {
                for (const int handle : entity.weapons()) {
                    if (handle == -1 || handle == 0)
                        continue;
                    const auto weapon = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntityFromHandle(handle));
                    if (!weapon.getPOD() || weapon.getWeaponType() == WeaponType::Knife)
                        continue;
                    const auto id = weapon.itemDefinitionIndex();
                    if (const auto icon = iconName(id))
                        iconGroups[order(weapon.getWeaponType())].push_back(icon);
                    if (id == WeaponId::C4) {
                        known.bomb = true;
                        continue;
                    }
                    if (const auto name = shortName(id))
                        groups[order(weapon.getWeaponType())].push_back(name);
                }
            }
            if (known.alive && known.armor > 0)
                iconGroups[2].insert(iconGroups[2].begin(), known.helmet ? "armor_helmet" : "armor");
            if (known.alive && known.kit)
                iconGroups[4].push_back("defuser");
            known.icons.clear();
            for (const auto& group : iconGroups) {
                for (const auto name : group) {
                    known.icons += known.icons.empty() ? "\"" : ",\"";
                    known.icons += name;
                    known.icons += '"';
                }
            }
            known.primary.clear();
            known.rest.clear();
            for (const auto& group : groups) {
                for (const auto name : group) {
                    if (known.primary.empty()) {
                        known.primary = name;
                        continue;
                    }
                    if (!known.rest.empty())
                        known.rest += " · ";
                    known.rest += name;
                }
            }
        }
        fresh.push_back(known);
        indices.push_back(i);
    }

    if (injectIcons(memory, interfaces, fresh, indices)) {
        std::scoped_lock lock{ mutex };
        visible = false;
        return;
    }
    // живые сверху, по деньгам — кто сейчас опаснее
    std::stable_sort(fresh.begin(), fresh.end(), [](const Entry& a, const Entry& b) {
        return a.alive != b.alive ? a.alive : a.money > b.money;
    });

    std::scoped_lock lock{ mutex };
    entries = std::move(fresh);
    visible = true;
}

void draw(ImDrawList* d) noexcept
{
    std::vector<Entry> list;
    {
        std::scoped_lock lock{ mutex };
        if (!visible || entries.empty())
            return;
        list = entries;
    }

    using namespace hvh::ui;
    ImFont* font = fonts.menu ? fonts.menu : ImGui::GetFont();
    ImFont* bold = fonts.menuBold ? fonts.menuBold : font;
    const float size = font->FontSize;
    const auto textWidth = [&](ImFont* f, const std::string& text) { return f->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x; };

    constexpr float padding = 8.0f, rowGap = 5.0f, border = 4.0f;
    const float lineHeight = size + 2.0f;
    const float rowHeight = lineHeight * 2.0f + rowGap;

    // ширина под самую длинную строку, но не шире трети экрана
    const auto& display = ImGui::GetIO().DisplaySize;
    float width = 230.0f;
    for (const auto& e : list) {
        width = std::max(width, textWidth(bold, e.name) + 120.0f);
        width = std::max(width, textWidth(font, e.primary) + textWidth(font, e.rest) + 30.0f);
    }
    width = std::min(width + padding * 2.0f, display.x * 0.33f);
    const float height = lineHeight + 6.0f + rowHeight * static_cast<float>(list.size()) + padding * 2.0f;

    // справа от таблицы (она по центру, примерно 2/3 высоты экрана в ширину)
    const ImVec2 pos{ std::min(display.x * 0.5f + display.y * 0.42f + 12.0f, display.x - width - border * 2.0f - 10.0f), display.y * 0.2f };
    const ImVec2 max{ pos.x + width + border * 2.0f, pos.y + height + border * 2.0f + 2.0f };

    // рамка как у меню и ватермарки
    d->AddRectFilled(pos, max, IM_COL32(0, 0, 0, 255));
    d->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { max.x - 1.0f, max.y - 1.0f }, IM_COL32(60, 60, 60, 255));
    d->AddRectFilled({ pos.x + 2.0f, pos.y + 2.0f }, { max.x - 2.0f, max.y - 2.0f }, IM_COL32(40, 40, 40, 255));
    d->AddRectFilled({ pos.x + border - 1.0f, pos.y + border - 1.0f }, { max.x - border + 1.0f, max.y - border + 1.0f }, IM_COL32(60, 60, 60, 255));
    const ImVec2 inner{ pos.x + border, pos.y + border };
    const ImVec2 innerMax{ max.x - border, max.y - border };
    d->AddRectFilled(inner, innerMax, IM_COL32(19, 19, 19, 240));
    const float mid = (inner.x + innerMax.x) * 0.5f;
    d->AddRectFilledMultiColor(inner, { mid, inner.y + 2.0f }, IM_COL32(55, 177, 218, 255), IM_COL32(204, 70, 205, 255), IM_COL32(204, 70, 205, 255), IM_COL32(55, 177, 218, 255));
    d->AddRectFilledMultiColor({ mid, inner.y }, { innerMax.x, inner.y + 2.0f }, IM_COL32(204, 70, 205, 255), IM_COL32(204, 227, 54, 255), IM_COL32(204, 227, 54, 255), IM_COL32(204, 70, 205, 255));

    const auto text = [&](ImFont* f, float x, float y, ImU32 col, const std::string& s) {
        d->AddText(f, size, { x + 1.0f, y + 1.0f }, IM_COL32(0, 0, 0, 200), s.c_str());
        d->AddText(f, size, { x, y }, col, s.c_str());
    };

    float y = inner.y + 2.0f + padding;
    const float left = inner.x + padding;
    const float right = innerMax.x - padding;
    text(bold, left, y, color::accent, "Снаряжение врагов");
    y += lineHeight + 6.0f;

    for (const auto& e : list) {
        const ImU32 nameColor = !e.alive ? color::textDim : e.dormant ? IM_COL32(170, 170, 170, 255) : IM_COL32(255, 255, 255, 255);
        // строка 1: ник ... хп / броня / кит / $
        char stats[64];
        if (e.alive)
            std::snprintf(stats, sizeof(stats), "%d HP  %s  $%d", e.health, e.armor > 0 ? (e.helmet ? "HK" : "K") : "—", e.money);
        else
            std::snprintf(stats, sizeof(stats), "мёртв  $%d", e.money);
        const float statsWidth = textWidth(font, stats);
        // ник обрезаем, чтобы не наезжал на правую колонку
        std::string name = e.name;
        while (!name.empty() && textWidth(bold, name) > right - left - statsWidth - 40.0f)
            name.pop_back();
        if (name.size() < e.name.size())
            name += "…";
        text(bold, left, y, nameColor, name);
        float tagX = left + textWidth(bold, name) + 6.0f;
        if (e.kit) {
            text(font, tagX, y, IM_COL32(106, 180, 255, 255), "KIT");
            tagX += textWidth(font, "KIT") + 5.0f;
        }
        if (e.bomb)
            text(font, tagX, y, IM_COL32(255, 106, 106, 255), "C4");
        const ImU32 hpColor = e.health > 66 ? color::good : e.health > 33 ? IM_COL32(230, 200, 80, 255) : color::bad;
        text(font, right - statsWidth, y, e.alive ? hpColor : color::textDim, stats);

        // строка 2: основное оружие акцентом, остальное серым
        const float y2 = y + lineHeight;
        if (e.alive) {
            const std::string primary = e.primary.empty() ? "нож" : e.primary;
            text(font, left + 8.0f, y2, color::accent, primary);
            if (!e.rest.empty())
                text(font, left + 8.0f + textWidth(font, primary) + 8.0f, y2, color::text, e.rest);
        } else {
            text(font, left + 8.0f, y2, color::textDim, "—");
        }
        y += rowHeight;
        if (&e != &list.back())
            d->AddLine({ left, y - rowGap * 0.5f }, { right, y - rowGap * 0.5f }, IM_COL32(44, 44, 44, 255));
    }
}

}
