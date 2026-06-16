#include "Menu.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "../font/resource.h"
#include <windows.h>

extern HMODULE g_hModule; // We'll need to define this in main.cpp

static int  g_lockDepth   = 0;
static bool g_lockRequest = false;

void Menu::PushLockZone(bool locked) { if (locked) ++g_lockDepth; }
void Menu::PopLockZone()             { if (g_lockDepth > 0) --g_lockDepth; }
static bool LockedNow() { return g_lockDepth > 0; }

static bool LockedConsumeClick(const ImRect& bb, ImGuiID id) {
    if (!LockedNow()) return false;
    bool hovered = false, held = false;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) g_lockRequest = true;
    return true;
}

static void DrawLockedRow(ImDrawList* dl, ImVec2 pos, float width, float rowH,
                          const char* label, const char* rightText) {
    ImU32 labelCol = IM_COL32(120, 122, 132, 220);
    ImU32 padlock  = IM_COL32(180, 70, 70, 230);
    dl->AddText(ImVec2(pos.x, pos.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f),
                labelCol, label);

    const char* tag = rightText ? rightText : "PAID";
    ImVec2 ts = ImGui::CalcTextSize(tag);
    float pillPad = 8.0f;
    float pillW = ts.x + pillPad * 2.0f;
    float pillH = 18.0f;
    ImVec2 pMin(pos.x + width - pillW - 2.0f, pos.y + (rowH - pillH) * 0.5f);
    ImVec2 pMax(pMin.x + pillW, pMin.y + pillH);
    dl->AddRectFilled(pMin, pMax, IM_COL32(48, 18, 22, 255), 4.0f);
    dl->AddRect(pMin, pMax, padlock, 4.0f, 0, 1.1f);
    dl->AddText(ImVec2(pMin.x + pillPad, pMin.y + (pillH - ts.y) * 0.5f),
                IM_COL32(245, 200, 200, 255), tag);
}

static const char* VkToName(int vk) {
    static char buf[32];
    switch (vk) {
        case 0x01: return "LMB";
        case 0x02: return "RMB";
        case 0x04: return "MMB";
        case 0x05: return "Mouse4";
        case 0x06: return "Mouse5";
        case VK_SHIFT: return "Shift";
        case VK_CONTROL: return "Ctrl";
        case VK_MENU: return "Alt";
        case VK_SPACE: return "Space";
        case VK_TAB: return "Tab";
        case VK_CAPITAL: return "Caps";
        case VK_ESCAPE: return "Esc";
        case 0: return "None";
    }
    if (vk >= 0x30 && vk <= 0x39) { snprintf(buf, sizeof(buf), "%c", vk); return buf; }
    if (vk >= 0x41 && vk <= 0x5A) { snprintf(buf, sizeof(buf), "%c", vk); return buf; }
    if (vk >= VK_F1 && vk <= VK_F12) { snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1); return buf; }
    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

static void DrawHelpIconAt(ImVec2 center, float r, const char* tooltip) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mp = ImGui::GetIO().MousePos;
    float dx = mp.x - center.x, dy = mp.y - center.y;
    bool hovered = (dx * dx + dy * dy) <= (r + 1.0f) * (r + 1.0f);
    ImU32 ringCol = hovered ? IM_COL32(180, 220, 255, 255) : IM_COL32(120, 180, 255, 220);
    ImU32 textCol = IM_COL32(120, 180, 255, 255);
    dl->AddCircle(center, r, ringCol, 12, 1.4f);
    char letter[2] = { 'i', 0 };
    ImVec2 ts = ImGui::CalcTextSize(letter);
    dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), textCol, letter);
    if (hovered && tooltip) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(360.0f);
        ImGui::TextUnformatted(tooltip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void HelpMarker(const char* tooltip) {
    ImGui::SameLine();
    ImVec2 cur = ImGui::GetCursorScreenPos();
    float r = ImGui::GetFontSize() * 0.42f;
    ImVec2 center = ImVec2(cur.x + r, cur.y + ImGui::GetFontSize() * 0.5f);
    DrawHelpIconAt(center, r, tooltip);
    ImGui::Dummy(ImVec2(r * 2.0f + 2.0f, ImGui::GetFontSize()));
}

static bool KeybindWidget(const char* label, int* vkCode) {
    static unsigned int s_capturing = 0;
    static uint8_t s_mustRelease[256] = { 0 };

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiID id = window->GetID(label);
    bool capturing = (s_capturing == (unsigned int)id);

    ImVec2 pos = window->DC.CursorPos;
    float totalWidth = ImGui::GetContentRegionAvail().x;
    float rowHeight = 26.0f;
    float boxW = 96.0f;
    float boxH = 22.0f;

    ImRect bb(pos, ImVec2(pos.x + totalWidth, pos.y + rowHeight));
    ImGui::ItemSize(bb, 0.0f);
    if (!ImGui::ItemAdd(bb, id)) return false;

    if (LockedConsumeClick(bb, id)) {
        DrawLockedRow(window->DrawList, pos, totalWidth, rowHeight, label, "PAID");
        return false;
    }

    ImVec2 boxMin(bb.Max.x - boxW - 4.0f, pos.y + (rowHeight - boxH) * 0.5f);
    ImVec2 boxMax(boxMin.x + boxW, boxMin.y + boxH);
    ImRect boxBb(boxMin, boxMax);

    bool hovered = false, held = false;
    bool pressed = ImGui::ButtonBehavior(boxBb, id, &hovered, &held);
    if (pressed) {
        if (capturing) {
            s_capturing = 0;
            capturing = false;
        } else {
            s_capturing = (unsigned int)id;
            for (int vk = 0x01; vk < 0xFF; ++vk) {
                s_mustRelease[vk] = ((GetAsyncKeyState(vk) & 0x8000) != 0) ? 1 : 0;
            }
            s_mustRelease[VK_LBUTTON] = 1;
            capturing = true;
        }
    }

    ImDrawList* dl = window->DrawList;
    ImU32 bgCol;
    ImU32 borderCol;
    if (capturing) {
        bgCol = IM_COL32(35, 25, 55, 255);
        borderCol = IM_COL32(160, 80, 240, 255);
    } else if (held) {
        bgCol = IM_COL32(28, 28, 36, 255);
        borderCol = IM_COL32(160, 80, 240, 200);
    } else if (hovered) {
        bgCol = IM_COL32(24, 24, 30, 255);
        borderCol = IM_COL32(110, 110, 130, 200);
    } else {
        bgCol = IM_COL32(18, 18, 24, 255);
        borderCol = IM_COL32(70, 70, 84, 180);
    }
    dl->AddRectFilled(boxMin, boxMax, bgCol, 5.0f);
    dl->AddRect(boxMin, boxMax, borderCol, 5.0f, 0, 1.2f);

    ImU32 textColor = IM_COL32(225, 225, 235, 255);

    if (capturing) {
        float t = (float)ImGui::GetTime();
        int active = ((int)(t * 3.0f)) % 4;
        float dotR = 2.2f;
        float spacing = 7.0f;
        float totalDotsW = 3 * dotR * 2.0f + 2 * spacing;
        float startX = boxMin.x + (boxW - totalDotsW) * 0.5f + dotR;
        float cy = boxMin.y + boxH * 0.5f;
        for (int i = 0; i < 3; ++i) {
            float alpha = (i == active) ? 1.0f : 0.30f;
            ImU32 dc = IM_COL32(200, 160, 255, (int)(alpha * 255));
            dl->AddCircleFilled(ImVec2(startX + i * (dotR * 2.0f + spacing), cy),
                                dotR, dc, 12);
        }
    } else {
        const char* keyName = VkToName(*vkCode);
        ImVec2 ts = ImGui::CalcTextSize(keyName);
        ImVec2 textPos(boxMin.x + (boxW - ts.x) * 0.5f,
                       boxMin.y + (boxH - ts.y) * 0.5f);
        dl->AddText(textPos, textColor, keyName);
    }

    ImU32 labelColor = IM_COL32(180, 185, 195, 255);
    dl->AddText(ImVec2(pos.x, pos.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f),
                labelColor, label);

    bool changed = false;
    if (capturing) {
        for (int vk = 0x01; vk < 0xFF; ++vk) {
            bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (s_mustRelease[vk] && !down) s_mustRelease[vk] = 0;
        }
        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) && !s_mustRelease[VK_ESCAPE]) {
            *vkCode = 0;
            s_capturing = 0;
            changed = true;
        } else {
            for (int vk = 0x01; vk < 0xFF; ++vk) {
                if (vk == VK_ESCAPE) continue;
                if (s_mustRelease[vk]) continue;
                if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
                    *vkCode = vk;
                    s_capturing = 0;
                    changed = true;
                    break;
                }
            }
        }
    }
    return changed;
}

static const uint32_t CFG_MAGIC   = 0x52484346;
static const uint32_t CFG_VERSION = 2;

struct CfgHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t dataSize;
};

std::string Menu::GetConfigDir() {
    char tempPath[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tempPath);
    std::string dir = std::string(tempPath) + "RainHack";
    std::filesystem::create_directories(dir);
    return dir;
}

bool Menu::SaveConfig(const char* name) {
    if (!name || !name[0]) return false;
    std::string path = GetConfigDir() + "\\" + name + ".rh";
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    CfgHeader hdr{ CFG_MAGIC, CFG_VERSION, 0 };
    uint32_t totalSize =
        sizeof(AimCfg) + sizeof(WeaponCfg) + sizeof(VisualCfg) +
        sizeof(AntiAimCfg) + sizeof(MiscCfg) + sizeof(WorldCfg) +
        sizeof(ParticleCfg) + sizeof(SettingsCfg);
    hdr.dataSize = totalSize;

    f.write((const char*)&hdr, sizeof(hdr));
    f.write((const char*)&AimCfg, sizeof(AimCfg));
    f.write((const char*)&WeaponCfg, sizeof(WeaponCfg));
    f.write((const char*)&VisualCfg, sizeof(VisualCfg));
    f.write((const char*)&AntiAimCfg, sizeof(AntiAimCfg));
    f.write((const char*)&MiscCfg, sizeof(MiscCfg));
    f.write((const char*)&WorldCfg, sizeof(WorldCfg));
    f.write((const char*)&ParticleCfg, sizeof(ParticleCfg));
    f.write((const char*)&SettingsCfg, sizeof(SettingsCfg));
    return f.good();
}

bool Menu::LoadConfig(const char* name) {
    if (!name || !name[0]) return false;
    std::string path = GetConfigDir() + "\\" + name + ".rh";
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    CfgHeader hdr{};
    f.read((char*)&hdr, sizeof(hdr));
    if (hdr.magic != CFG_MAGIC || hdr.version != CFG_VERSION) return false;

    f.read((char*)&AimCfg, sizeof(AimCfg));
    f.read((char*)&WeaponCfg, sizeof(WeaponCfg));
    f.read((char*)&VisualCfg, sizeof(VisualCfg));
    f.read((char*)&AntiAimCfg, sizeof(AntiAimCfg));
    f.read((char*)&MiscCfg, sizeof(MiscCfg));
    f.read((char*)&WorldCfg, sizeof(WorldCfg));
    f.read((char*)&ParticleCfg, sizeof(ParticleCfg));
    f.read((char*)&SettingsCfg, sizeof(SettingsCfg));
    return f.good();
}

bool Menu::DeleteConfig(const char* name) {
    if (!name || !name[0]) return false;
    std::string path = GetConfigDir() + "\\" + name + ".rh";
    return std::filesystem::remove(path);
}

std::vector<std::string> Menu::GetConfigList() {
    std::vector<std::string> list;
    std::string dir = GetConfigDir();
    if (!std::filesystem::exists(dir)) return list;
    for (auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".rh") {
            list.push_back(entry.path().stem().string());
        }
    }
    std::sort(list.begin(), list.end());
    return list;
}

void Menu::SetVisible(bool v) {
    auto& me = Get();
    if (me.m_isOpen == v) return;
    me.m_isOpen = v;
    if (v) {
        me.m_openAnim    = 1.0f;
        me.m_contentAlpha = 1.0f;
        me.m_lastFrame   = std::chrono::steady_clock::now();
    }
}

bool Menu::IsVisible() {
    return Get().m_isOpen;
}

void Menu::Initialize() {
    auto& me = Get();
    me.m_lastFrame   = std::chrono::steady_clock::now();
    me.m_currentTab  = Tab::Aim;
    me.m_isOpen      = true;
    me.m_openAnim    = 1.0f;
    me.m_contentAlpha = 1.0f;
    me.m_tabIndicatorX = 0.0f;
    me.m_tabIndicatorTargetX = 0.0f;
    me.m_toggleAnims.clear();
    me.m_hoverAnims.clear();
    
    // Load font from resources
    ImGuiIO& io = ImGui::GetIO();
    HRSRC hRes = FindResourceA(g_hModule, MAKEINTRESOURCEA(IDR_FONT1), RT_RCDATA);
    if (hRes) {
        HGLOBAL hData = LoadResource(g_hModule, hRes);
        if (hData) {
            void* pFontData = LockResource(hData);
            DWORD dwFontSize = SizeofResource(g_hModule, hRes);
            if (pFontData && dwFontSize > 0) {
                // We must make a copy of the font data if we want ImGui to manage it, 
                // or use FontDataOwnedByAtlas = false. 
                // Using memory from resource is fine if DLL is not unloaded.
                ImFontConfig font_cfg;
                font_cfg.FontDataOwnedByAtlas = false;
                io.Fonts->AddFontFromMemoryTTF(pFontData, dwFontSize, 18.0f, &font_cfg);
            }
        }
    }

    SetStyle();
}

void Menu::RenderVisualsTab() {
    auto& me   = Get();
    auto& cfg  = me.VisualCfg;
    auto& pl   = cfg.Player;
    auto& npc  = cfg.NPC;
    auto& veh  = cfg.Vehicle;
    auto& dep  = cfg.Deployable;

    ImGui::Indent(12.0f);
    me.SectionHeader("ESP CATEGORY");
    const char* categories[] = { "Players", "NPCs", "Vehicles", "Deployables", "Chams" };
    ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.45f);
    me.CustomCombo("##VisualCategory", &cfg.SelectedCategory, categories, 5);
    ImGui::PopItemWidth();
    ImGui::Unindent(4.0f);

    ImGui::Spacing();

    if (cfg.SelectedCategory == 0) {
        pl.DrawTeam    = false;
        pl.DrawEnemies = true;

        ImGui::Columns(2, "PlayerVisCols", false);
        ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.5f);

        me.SectionHeader("PLAYERS");
        me.CustomToggle("Enable Player ESP", &pl.Enabled);

        if (!pl.Enabled) {
            ImGui::Columns(1);
            return;
        }

        const char* boxTypes[] = { "Off", "2D Box", "Corner" };
        me.CustomCombo("Box Type", &pl.BoxType, boxTypes, 3);

        me.CustomToggle("Name",            &pl.Name);
        me.CustomToggle("Distance",        &pl.Distance);
        me.CustomToggle("Team ID",         &pl.TeamID);
        me.CustomToggle("Health Bar",      &pl.Health);
        me.CustomToggle("Held Item",       &pl.Weapon);
        me.CustomToggle("View Direction",  &pl.ViewDirection);
        me.CustomToggle("Skeleton",        &pl.Skeleton);
        if (pl.Skeleton) {
            me.CustomSliderFloat("Skeleton Thickness", &pl.SkeletonThickness, 0.5f, 4.0f, "%.1f px");
        }
        me.CustomToggle("Target Line",     &pl.TargetLine);
        me.CustomToggle("Target Hotbar",   &pl.TargetBelt);
        me.CustomToggle("OOF Indicator",   &pl.OOFIndicator);
        me.CustomToggle("Outside Mark",    &pl.OutsideMark);

        me.SectionHeader("FILTERS");
        Menu::PushLockZone(true);
        me.CustomToggle("Draw Teammates",       &pl.DrawTeam);
        Menu::PopLockZone();
        me.CustomToggle("Draw Enemies",         &pl.DrawEnemies);
        me.CustomToggle("Draw Wounded",         &pl.DrawWounded);
        me.CustomToggle("Draw Dead",            &pl.DrawDead);
        me.CustomToggle("Draw Sleeping",        &pl.DrawSleeping);
        if (pl.DrawSleeping) {
            ImGui::Indent(14.0f);
            me.CustomToggle("Online Sleepers Only", &pl.OnlineSleeperOnly);
            ImGui::Unindent(14.0f);
        }
        me.CustomToggle("Safezone Players",     &pl.DrawSafezone);

        ImGui::NextColumn();

        me.SectionHeader("COLORS");
        me.CustomToggle("Use Visible Color", &pl.UseVisibleColor);
        if (pl.UseVisibleColor) {
            me.CustomColorEdit("Visible",  pl.ColorVisible);
        }
        me.CustomColorEdit("Enemy",     pl.ColorEnemy);
        me.CustomColorEdit("Team",      pl.ColorTeam);
        me.CustomColorEdit("Sleeping",  pl.ColorSleeping);
        me.CustomColorEdit("Wounded",   pl.ColorWounded);
        me.CustomColorEdit("Dead",      pl.ColorDead);
        if (pl.OOFIndicator) {
            me.CustomColorEdit("OOF Arrow", pl.ColorOOF);
        }

        me.SectionHeader("LAYOUT / RANGE");
        me.CustomSliderFloat("Text Spacing",  &pl.TextSpacing,  8.0f, 28.0f, "%.0f px");
        me.CustomSliderFloat("Draw Distance", &pl.DrawDistance, 25.0f, 1500.0f, "%.0f m");
        if (pl.OOFIndicator) {
            me.CustomSliderFloat("OOF Radius", &pl.OOFRadius, 80.0f, 480.0f, "%.0f px");
        }

        ImGui::Columns(1);
    }
    else if (cfg.SelectedCategory == 1) {
        Menu::PushLockZone(true);
        ImGui::Columns(2, "NPCVisCols", false);
        ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.5f);

        me.SectionHeader("NPCs / SCIENTISTS");
        me.CustomToggle("Enable NPC ESP", &npc.Enabled);

        if (!npc.Enabled) {
            ImGui::Columns(1);
            Menu::PopLockZone();
            return;
        }

        const char* boxTypes[] = { "Off", "2D Box", "Corner" };
        me.CustomCombo("Box Type", &npc.BoxType, boxTypes, 3);

        me.CustomToggle("Name",        &npc.Name);
        me.CustomToggle("Distance",    &npc.Distance);
        me.CustomToggle("Health Bar",  &npc.Health);
        me.CustomToggle("Held Item",   &npc.Weapon);
        me.CustomToggle("Target Line", &npc.TargetLine);
        me.CustomToggle("Skeleton",    &npc.Skeleton);
        if (npc.Skeleton) {
            me.CustomSliderFloat("Skeleton Thickness", &npc.SkeletonThickness, 0.5f, 4.0f, "%.1f px");
        }

        ImGui::NextColumn();

        me.SectionHeader("APPEARANCE");
        me.CustomColorEdit("NPC Color", npc.Color);

        me.SectionHeader("LAYOUT / RANGE");
        me.CustomSliderFloat("Text Spacing",  &npc.TextSpacing,  8.0f, 28.0f, "%.0f px");
        me.CustomSliderFloat("Draw Distance", &npc.DrawDistance, 25.0f, 1500.0f, "%.0f m");

        ImGui::Columns(1);
        Menu::PopLockZone();
    }
    else if (cfg.SelectedCategory == 2) {
        Menu::PushLockZone(true);
        ImGui::Columns(2, "VehicleVisCols", false);
        ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.5f);

        me.SectionHeader("VEHICLES");
        me.CustomToggle("Enable Vehicle ESP", &veh.Enabled);

        if (!veh.Enabled) {
            ImGui::Columns(1);
            Menu::PopLockZone();
            return;
        }

        const char* boxTypes[] = { "Off", "2D Box", "Corner" };
        me.CustomCombo("Box Type", &veh.BoxType, boxTypes, 3);

        me.CustomToggle("Name",       &veh.Name);
        me.CustomToggle("Distance",   &veh.Distance);
        me.CustomToggle("Health",     &veh.Health);

        me.SectionHeader("APPEARANCE");
        me.CustomColorEdit("Color",   veh.Color);
        me.CustomSliderFloat("Draw Distance", &veh.DrawDistance, 50.0f, 4000.0f, "%.0f m");

        ImGui::NextColumn();

        me.SectionHeader("VEHICLE TYPES");
        me.CustomToggle("Patrol Heli",       &veh.PatrolHeli);
        me.CustomToggle("Bradley APC",       &veh.Bradley);
        me.CustomToggle("Drones",            &veh.Drones);
        me.CustomToggle("Cargo Ship",        &veh.CargoShip);
        me.CustomToggle("Minicopter",        &veh.Minicopter);
        me.CustomToggle("Scrap Heli",        &veh.ScrapHeli);
        me.CustomToggle("Attack Helicopter", &veh.AttackHeli);
        me.CustomToggle("Rowboat",           &veh.Rowboat);
        me.CustomToggle("Submarine",         &veh.Submarine);
        me.CustomToggle("Tug Boat",          &veh.TugBoat);
        me.CustomToggle("Bikes",             &veh.Bikes);

        ImGui::Columns(1);
        Menu::PopLockZone();
    }
    else if (cfg.SelectedCategory == 3) {
        Menu::PushLockZone(true);
        ImGui::Columns(2, "DeployVisCols", false);
        ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.5f);

        me.SectionHeader("DEPLOYABLES");
        me.CustomToggle("Enable Deployable ESP", &dep.Enabled);

        if (!dep.Enabled) {
            ImGui::Columns(1);
            Menu::PopLockZone();
            return;
        }

        const char* boxTypes[] = { "Off", "2D Box", "Corner" };
        me.CustomCombo("Box Type", &dep.BoxType, boxTypes, 3);

        me.CustomToggle("Name",     &dep.Name);
        me.CustomToggle("Distance", &dep.Distance);

        me.SectionHeader("APPEARANCE");
        me.CustomColorEdit("Color", dep.Color);
        me.CustomSliderFloat("Draw Distance", &dep.DrawDistance, 25.0f, 800.0f, "%.0f m");

        ImGui::NextColumn();

        me.SectionHeader("DEPLOYABLE TYPES");
        me.CustomToggle("Recycler",        &dep.Recycler);
        me.CustomToggle("Cupboard (TC)",   &dep.Cupboard);
        if (dep.Cupboard) {
            ImGui::Indent(14.0f);
            me.CustomToggle("TC HealthBar", &dep.TC_HealthBar);
            me.CustomToggle("TC Upkeep",    &dep.TC_Upkeep);
            me.CustomToggle("TC Show ID",   &dep.TC_ShowID);
            ImGui::Unindent(14.0f);
        }
        me.CustomToggle("Stashes",         &dep.Stashes);
        me.CustomToggle("Sleeping Bag",    &dep.SleepingBag);
        me.CustomToggle("RF Receiver",     &dep.RFReceiver);
        me.CustomToggle("RF Broadcaster",  &dep.RFBroadcaster);
        me.CustomToggle("HBHF Sensor",     &dep.HBHFSensor);
        me.CustomToggle("Seismic Sensor",  &dep.SeismicSensor);
        me.CustomToggle("Large Battery",   &dep.LargeBattery);
        me.CustomToggle("Workbench T2/T3", &dep.Workbench);

        ImGui::Columns(1);
        Menu::PopLockZone();
    }
    else if (cfg.SelectedCategory == 4) {
        Menu::PushLockZone(true);
        auto& ch = cfg.Chams;

        const char* subTabs[] = { "Players", "Friendly", "NPCs", "Vehicles", "Deployables", "Misc" };
        const int subCount = 6;
        const float btnW = 94.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        float totalW = btnW * subCount + spacing * (subCount - 1);
        float availW = ImGui::GetContentRegionAvail().x;
        if (totalW < availW)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - totalW) * 0.5f);
        ImGui::BeginGroup();
        for (int i = 0; i < subCount; i++) {
            if (i > 0) ImGui::SameLine();
            bool sel = (ch.SubTab == i);
            ImVec4 baseAcc = me.GetAccentColor();
            ImVec4 inactive(0.18f, 0.18f, 0.22f, 1.00f);
            ImGui::PushStyleColor(ImGuiCol_Button,
                sel ? ImVec4(baseAcc.x * 0.9f, baseAcc.y * 0.9f, baseAcc.z * 0.9f, 1.0f) : inactive);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                sel ? ImVec4(baseAcc.x, baseAcc.y, baseAcc.z, 1.0f)
                    : ImVec4(0.26f, 0.26f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(baseAcc.x * 1.1f, baseAcc.y * 1.1f, baseAcc.z * 1.1f, 1.0f));
            if (ImGui::Button(subTabs[i], ImVec2(94, 26))) ch.SubTab = i;
            ImGui::PopStyleColor(3);
        }
        ImGui::EndGroup();
        ImGui::Spacing();

        auto RenderChamsBlock = [&](const char* header, Menu::VisualConfig::ChamsSettings& cs) {
            ImGui::Columns(2, "ChamsCols", false);
            ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.5f);

            me.SectionHeader(header);
            me.CustomToggle("Enable Chams", &cs.Enabled);

            if (cs.Enabled) {
                const char* styles[] = { "Flat", "Standard", "Glow", "Wireframe" };
                me.CustomCombo("Material Style", &cs.Style, styles, 4);
            }

            ImGui::NextColumn();

            me.SectionHeader("COLORS");
            if (cs.Enabled) {
                me.CustomColorEdit("Visible",  cs.VisColor);
                me.CustomColorEdit("Occluded", cs.OccColor);
            }

            ImGui::Columns(1);
        };

        switch (ch.SubTab) {
            case 0: RenderChamsBlock("PLAYER CHAMS",     ch.Player);     break;
            case 1: RenderChamsBlock("FRIENDLY CHAMS",   ch.Friendly);   break;
            case 2: RenderChamsBlock("NPC CHAMS",        ch.NPC);        break;
            case 3: RenderChamsBlock("VEHICLE CHAMS",    ch.Vehicle);    break;
            case 4: RenderChamsBlock("DEPLOYABLE CHAMS", ch.Deployable); break;
            case 5: {
                ImGui::Columns(2, "ChamsMisc", false);
                ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.5f);

                me.SectionHeader("MISC");
                me.CustomToggle("Disable Player Culling", &ch.DisablePlayerCulling);

                ImGui::NextColumn();

                me.SectionHeader("INFO");
                ImGui::TextWrapped(
                    "Disable Player Culling forces the highest LOD on every visible player so distant or partially-occluded players keep rendering instead of being culled by Unity's LODGroup system.");
                ImGui::Spacing();
                ImGui::TextWrapped(
                    "Material styles: Flat = unlit solid, Standard = depth-tested PBR, Glow = emissive overlay, Wireframe = wire pass.");
                ImGui::Columns(1);
                break;
            }
        }
        Menu::PopLockZone();
    }
}

void Menu::SetStyle() {
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 12.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;

    style.WindowPadding = ImVec2(0, 0);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 6);
    style.ItemInnerSpacing = ImVec2(8, 4);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 8.0f;

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.45f, 0.47f, 0.52f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.07f, 0.07f, 0.09f, 0.96f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.10f, 0.10f, 0.13f, 0.96f);
    colors[ImGuiCol_Border]                = ImVec4(0.20f, 0.20f, 0.24f, 0.40f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.16f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.07f, 0.07f, 0.09f, 0.40f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.25f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.35f, 0.35f, 0.40f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.45f, 0.45f, 0.50f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.63f, 0.13f, 0.94f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.75f, 0.25f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.63f, 0.13f, 0.94f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.63f, 0.13f, 0.94f, 0.50f);
    colors[ImGuiCol_Separator]             = ImVec4(0.20f, 0.20f, 0.24f, 0.50f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.63f, 0.13f, 0.94f, 0.40f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.63f, 0.13f, 0.94f, 1.00f);
}

float Menu::Lerp(float a, float b, float t) {
    t = (std::max)(0.0f, (std::min)(1.0f, t));
    return a + (b - a) * t;
}

ImVec4 Menu::GetAccentColor() {
    return ImVec4(0.63f, 0.13f, 0.94f, 1.00f); 
}

ImU32 Menu::GetAccentColorU32(float alpha) {
    ImVec4 c = GetAccentColor();
    c.w = alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

bool Menu::CustomToggle(const char* label, bool* v) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiID id = window->GetID(label);
    ImVec2 pos = window->DC.CursorPos;
    float totalWidth = ImGui::GetContentRegionAvail().x;
    
    float rowHeight = 24.0f;
    float toggleW = 44.0f;
    float toggleH = 22.0f;
    float toggleRadius = toggleH * 0.5f;
    
    ImRect bb(pos, ImVec2(pos.x + totalWidth, pos.y + rowHeight));
    ImGui::ItemSize(bb, 0.0f);
    if (!ImGui::ItemAdd(bb, id)) return false;

    if (LockedConsumeClick(bb, id)) {
        DrawLockedRow(window->DrawList, pos, totalWidth, rowHeight, label, "PAID");
        return false;
    }

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) *v = !*v;

    auto& anim = m_toggleAnims[id];
    float target = *v ? 1.0f : 0.0f;
    anim.knobPos = Lerp(anim.knobPos, target, m_deltaTime * 14.0f);
    anim.bgColor = Lerp(anim.bgColor, target, m_deltaTime * 12.0f);

    float& hoverAnim = m_hoverAnims[id];
    hoverAnim = Lerp(hoverAnim, hovered ? 1.0f : 0.0f, m_deltaTime * 12.0f);

    ImDrawList* dl = window->DrawList;
    float t = anim.bgColor;
    
    float toggleX = bb.Max.x - toggleW - 4.0f;
    float toggleY = pos.y + (rowHeight - toggleH) * 0.5f;
    ImVec2 tMin(toggleX, toggleY);
    ImVec2 tMax(toggleX + toggleW, toggleY + toggleH);

    ImVec4 accent = GetAccentColor();
    float bgR = Lerp(0.18f, accent.x, t);
    float bgG = Lerp(0.18f, accent.y, t);
    float bgB = Lerp(0.20f, accent.z, t);
    ImU32 bgCol = ImGui::ColorConvertFloat4ToU32(ImVec4(bgR, bgG, bgB, 0.85f + 0.15f * t));
    
    if (t > 0.05f) {
        dl->AddRectFilled(
            ImVec2(tMin.x - 3, tMin.y - 3),
            ImVec2(tMax.x + 3, tMax.y + 3),
            GetAccentColorU32(0.10f * t), toggleRadius + 3);
    }
    
    dl->AddRectFilled(tMin, tMax, bgCol, toggleRadius);

    ImU32 borderCol = t > 0.5f ? GetAccentColorU32(0.4f + 0.2f * hoverAnim)
                                : IM_COL32(60, 60, 68, (int)(180 + 75 * hoverAnim));
    dl->AddRect(tMin, tMax, borderCol, toggleRadius, 0, 1.2f);

    float knobPad = 3.0f;
    float knobR = (toggleH - knobPad * 2) * 0.5f;
    float knobTravel = toggleW - knobPad * 2 - knobR * 2;
    float knobCX = toggleX + knobPad + knobR + anim.knobPos * knobTravel;
    float knobCY = toggleY + toggleH * 0.5f;
    float knobScale = 1.0f + hoverAnim * 0.08f;

    dl->AddCircleFilled(ImVec2(knobCX + 1, knobCY + 1), knobR * knobScale + 1.5f,
        IM_COL32(0, 0, 0, (int)(40 + 20 * t)), 24);

    ImU32 knobCol = IM_COL32(
        (int)(220 + 35 * t),
        (int)(220 + 35 * t),
        (int)(225 + 30 * t), 255);
    dl->AddCircleFilled(ImVec2(knobCX, knobCY), knobR * knobScale, knobCol, 24);
    
    if (t > 0.3f) {
        float s = knobR * 0.35f * t;
        ImU32 checkCol = GetAccentColorU32(t * 0.9f);
        ImVec2 p1(knobCX - s * 0.6f, knobCY + s * 0.1f);
        ImVec2 p2(knobCX - s * 0.1f, knobCY + s * 0.55f);
        ImVec2 p3(knobCX + s * 0.7f, knobCY - s * 0.45f);
        dl->AddLine(p1, p2, checkCol, 1.8f);
        dl->AddLine(p2, p3, checkCol, 1.8f);
    }

    ImU32 textColor = IM_COL32(
        (int)(150 + 90 * t),
        (int)(155 + 85 * t),
        (int)(165 + 75 * t), 255);
    dl->AddText(ImVec2(pos.x, pos.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f), textColor, label);

    return pressed;
}

bool Menu::CustomToggle(const char* label, bool* v, const char* helpTooltip) {
    ImVec2 rowStart = ImGui::GetCursorScreenPos();
    float rowH = 24.0f;
    bool pressed = CustomToggle(label, v);
    if (helpTooltip) {
        ImVec2 ts = ImGui::CalcTextSize(label);
        float r = ImGui::GetFontSize() * 0.42f;
        ImVec2 center = ImVec2(
            rowStart.x + ts.x + 8.0f + r,
            rowStart.y + rowH * 0.5f);
        DrawHelpIconAt(center, r, helpTooltip);
    }
    return pressed;
}

bool Menu::CustomSliderFloat(const char* label, float* v, float v_min, float v_max, const char* fmt) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiID id = window->GetID(label);
    float totalWidth = ImGui::GetContentRegionAvail().x;
    float height = 28.0f;
    float trackHeight = 6.0f;
    float knobRadius = 7.0f;
    
    ImVec2 pos = window->DC.CursorPos;
    ImRect bb(pos, ImVec2(pos.x + totalWidth, pos.y + height));
    ImGui::ItemSize(bb, 0.0f);
    if (!ImGui::ItemAdd(bb, id)) return false;

    if (LockedConsumeClick(bb, id)) {
        DrawLockedRow(window->DrawList, pos, totalWidth, height, label, "PAID");
        return false;
    }

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    
    bool changed = false;
    float trackLeft = pos.x;
    float trackRight = pos.x + totalWidth - 1.0f;
    float trackWidth = trackRight - trackLeft;
    
    if (held) {
        float mouseX = ImGui::GetIO().MousePos.x;
        float ratio = (mouseX - trackLeft) / trackWidth;
        ratio = (std::max)(0.0f, (std::min)(1.0f, ratio));
        float newVal = v_min + ratio * (v_max - v_min);
        if (newVal != *v) { *v = newVal; changed = true; }
    }

    float t = (*v - v_min) / (v_max - v_min);
    t = (std::max)(0.0f, (std::min)(1.0f, t));

    float& hoverAnim = m_hoverAnims[id];
    hoverAnim = Lerp(hoverAnim, (hovered || held) ? 1.0f : 0.0f, m_deltaTime * 12.0f);

    ImDrawList* dl = window->DrawList;
    
    ImU32 labelCol = IM_COL32(180, 185, 195, 255);
    dl->AddText(ImVec2(pos.x, pos.y), labelCol, label);
    
    char valueBuf[64];
    snprintf(valueBuf, sizeof(valueBuf), fmt, *v);
    ImVec2 valSize = ImGui::CalcTextSize(valueBuf);
    dl->AddText(ImVec2(pos.x + totalWidth - valSize.x, pos.y), GetAccentColorU32(0.9f), valueBuf);
    
    float trackY = pos.y + height - trackHeight - 2.0f;
    
    dl->AddRectFilled(
        ImVec2(trackLeft, trackY),
        ImVec2(trackRight, trackY + trackHeight),
        IM_COL32(30, 30, 36, 255), trackHeight * 0.5f);
    
    float fillRight = trackLeft + t * trackWidth;
    
    float intensity = 0.45f + 0.55f * t;
    ImVec4 baseAccent = GetAccentColor();
    ImU32 dynamicCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
        baseAccent.x * intensity,
        baseAccent.y * intensity,
        baseAccent.z * intensity, 1.0f));

    if (fillRight > trackLeft + 1.0f) {
        ImU32 darkCol = ImGui::ColorConvertFloat4ToU32(ImVec4(baseAccent.x * 0.3f, baseAccent.y * 0.3f, baseAccent.z * 0.3f, 1.0f));
        ImU32 brightCol = ImGui::ColorConvertFloat4ToU32(ImVec4(baseAccent.x * 1.0f, baseAccent.y * 1.0f, baseAccent.z * 1.0f, 1.0f));
        
        dl->AddRectFilledMultiColor(
            ImVec2(trackLeft, trackY),
            ImVec2(fillRight, trackY + trackHeight),
            darkCol, brightCol, brightCol, darkCol);
        
        ImU32 darkGlow = ImGui::ColorConvertFloat4ToU32(ImVec4(baseAccent.x * 0.3f, baseAccent.y * 0.3f, baseAccent.z * 0.3f, 0.15f));
        ImU32 brightGlow = ImGui::ColorConvertFloat4ToU32(ImVec4(baseAccent.x * 1.0f, baseAccent.y * 1.0f, baseAccent.z * 1.0f, 0.15f));
        
        dl->AddRectFilledMultiColor(
            ImVec2(trackLeft, trackY - 1),
            ImVec2(fillRight, trackY + trackHeight + 1),
            darkGlow, brightGlow, brightGlow, darkGlow);
    }
    
    float knobX = trackLeft + t * trackWidth;
    float knobY = trackY + trackHeight * 0.5f;
    float currentKnobR = knobRadius + hoverAnim * 2.0f;
    
    dl->AddCircleFilled(ImVec2(knobX, knobY), currentKnobR + 2.0f,
        GetAccentColorU32(0.15f + 0.1f * hoverAnim), 20);
    dl->AddCircleFilled(ImVec2(knobX, knobY), currentKnobR,
        IM_COL32(255, 255, 255, 240), 20);
    dl->AddCircleFilled(ImVec2(knobX, knobY), currentKnobR * 0.45f,
        dynamicCol, 16);

    return changed;
}

bool Menu::CustomSliderInt(const char* label, int* v, int v_min, int v_max) {
    float fv = (float)*v;
    bool changed = CustomSliderFloat(label, &fv, (float)v_min, (float)v_max, "%.0f");
    *v = (int)fv;
    return changed;
}

bool Menu::CustomCombo(const char* label, int* current_item, const char* const items[], int items_count) {
    if (LockedNow()) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (!window->SkipItems) {
            ImGuiID id = window->GetID(label);
            ImVec2 pos = window->DC.CursorPos;
            float w = ImGui::GetContentRegionAvail().x;
            float h = 26.0f;
            ImRect bb(pos, ImVec2(pos.x + w, pos.y + h));
            ImGui::ItemSize(bb, 0.0f);
            if (ImGui::ItemAdd(bb, id)) {
                LockedConsumeClick(bb, id);
                DrawLockedRow(window->DrawList, pos, w, h, label, "PAID");
            }
        }
        return false;
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.14f, 0.14f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.10f, 0.10f, 0.13f, 0.98f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    bool changed = ImGui::Combo(label, current_item, items, items_count);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    return changed;
}

void Menu::CustomColorEdit(const char* label, float col[4]) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;

    ImGui::PushID(label);
    
    float width = ImGui::GetContentRegionAvail().x;
    float height = 24.0f;
    ImVec2 pos = window->DC.CursorPos;
    ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));
    ImGui::ItemSize(bb, 0.0f);
    ImGuiID rowId = window->GetID(label);
    if (!ImGui::ItemAdd(bb, rowId)) {
        ImGui::PopID();
        return;
    }

    if (LockedConsumeClick(bb, rowId)) {
        DrawLockedRow(window->DrawList, pos, width, height, label, "PAID");
        ImGui::PopID();
        return;
    }

    window->DrawList->AddText(ImVec2(pos.x, pos.y + (height - ImGui::GetTextLineHeight()) * 0.5f), 
        IM_COL32(190, 195, 205, 255), label);

    float btnW = 32.0f;
    float btnH = 18.0f;
    ImVec2 btnPos(bb.Max.x - btnW - 4.0f, pos.y + (height - btnH) * 0.5f);
    
    ImGui::SetCursorScreenPos(btnPos);
    bool pressed = ImGui::ColorButton("##ColorBtn", *(ImVec4*)col, 
        ImGuiColorEditFlags_AlphaPreview | ImGuiColorEditFlags_NoTooltip, ImVec2(btnW, btnH));
    
    if (pressed) {
        ImGui::OpenPopup("ColorPickerPopup");
    }

    if (ImGui::BeginPopupContextItem("##ColorContext")) {
        static float clipboard[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        if (ImGui::MenuItem("Copy")) memcpy(clipboard, col, sizeof(float) * 4);
        if (ImGui::MenuItem("Paste")) memcpy(col, clipboard, sizeof(float) * 4);
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("ColorPickerPopup")) {
        ImGui::ColorPicker4("##Picker", col, 
            ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_AlphaPreview);
        ImGui::EndPopup();
    }

    ImGui::PopID();
}

void Menu::SectionHeader(const char* label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec4 accent = GetAccentColor();
    dl->AddRectFilled(pos, ImVec2(pos.x + 3.0f, pos.y + ImGui::GetTextLineHeight()), GetAccentColorU32());
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);
    ImGui::TextColored(ImVec4(accent.x, accent.y, accent.z, 0.9f), "%s", label);
}

void Menu::RenderHeader(ImDrawList* dl, ImVec2 pos, ImVec2 size) {
    (void)dl; (void)pos; (void)size;
}

void Menu::RenderTabBar(ImDrawList* dl, ImVec2 pos, ImVec2 size) {
    ImU32 barLeft = GetAccentColorU32(0.12f);
    ImU32 barRight = IM_COL32(14, 14, 18, 255);
    dl->AddRectFilledMultiColor(pos, ImVec2(pos.x + size.x, pos.y + size.y),
        barLeft, barRight, barRight, barLeft);

    const char* tabNames[] = { "Aim", "Anti-Aim", "Weapon", "Visual", "World", "Misc", "Settings" };
    const int tabCount = 7;

    float tabW = (size.x - 10.0f) / (float)tabCount;
    float startX = pos.x + 5.0f;

    m_tabIndicatorTargetX = startX + (int)m_currentTab * tabW;
    m_tabIndicatorX = Lerp(m_tabIndicatorX, m_tabIndicatorTargetX, m_deltaTime * 14.0f);

    for (int i = 0; i < tabCount; i++) {
        float tx = startX + i * tabW;
        float ty = pos.y;
        bool selected = (m_currentTab == (Tab)i);

        ImRect tabRect(ImVec2(tx, ty), ImVec2(tx + tabW, ty + size.y));
        ImGuiID tabId = ImGui::GetID(tabNames[i]);
        
        bool hovered = ImGui::IsMouseHoveringRect(tabRect.Min, tabRect.Max);
        if (hovered && ImGui::IsMouseClicked(0)) {
            if (m_currentTab != (Tab)i) {
                m_contentAlpha = 0.0f;
            }
            m_currentTab = (Tab)i;
        }

        float& hoverA = m_hoverAnims[tabId];
        hoverA = Lerp(hoverA, hovered ? 1.0f : 0.0f, m_deltaTime * 10.0f);
        
        if (hoverA > 0.01f && !selected) {
            dl->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + tabW, ty + size.y),
                IM_COL32(255, 255, 255, (int)(8 * hoverA)));
        }

        ImVec2 textSize = ImGui::CalcTextSize(tabNames[i]);
        float textX = tx + (tabW - textSize.x) * 0.5f;
        float textY = ty + (size.y - textSize.y) * 0.5f;
        
        ImU32 textCol = selected ? IM_COL32(255, 255, 255, 245)
                                 : IM_COL32(140, 145, 155, (int)(180 + 75 * hoverA));
        dl->AddText(ImVec2(textX, textY), textCol, tabNames[i]);
    }

    float indicatorW = tabW - 20;
    dl->AddRectFilled(
        ImVec2(m_tabIndicatorX + 10, pos.y + size.y - 3),
        ImVec2(m_tabIndicatorX + 10 + indicatorW, pos.y + size.y),
        GetAccentColorU32(1.0f), 2.0f);
    
    dl->AddRectFilled(
        ImVec2(m_tabIndicatorX + 10, pos.y + size.y - 6),
        ImVec2(m_tabIndicatorX + 10 + indicatorW, pos.y + size.y),
        GetAccentColorU32(0.2f), 2.0f);

    dl->AddLine(ImVec2(pos.x, pos.y + size.y), ImVec2(pos.x + size.x, pos.y + size.y),
        IM_COL32(30, 30, 38, 255));
}

void Menu::RenderContent() {
    auto& me = Get();
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 10));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.09f, 0.0f));
    ImGui::BeginChild("ContentArea##main", ImVec2(0, 0), false);
    
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, me.m_contentAlpha);

    switch (me.m_currentTab) {
    case Tab::Aim: {
        Menu::PushLockZone(true);
        ImGui::Columns(2, "AimColumns", false);
        ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.55f);

        me.SectionHeader("AIM ASSIST");
        me.CustomToggle("Aimbot", &me.AimCfg.Enabled);
        me.CustomToggle("Perfect Silent", &me.AimCfg.PerfectSilent);
        
        const char* hitboxes[] = { "Head", "Neck", "Chest", "Pelvis" };
        me.CustomCombo("Target Hitbox", &me.AimCfg.Hitbox, hitboxes, 4);
        
        me.CustomSliderFloat("FOV Range", &me.AimCfg.FOV, 1.0f, 2000.0f, "%.0f");
        me.CustomToggle("Draw FOV", &me.AimCfg.ShowFOV);

        me.SectionHeader("TARGET FILTERS");
        me.CustomToggle("Visible Check", &me.AimCfg.VisibleCheck);
        me.CustomToggle("Target NPC", &me.AimCfg.TargetNPC);
        me.CustomToggle("Target Sleeping", &me.AimCfg.TargetSleeping);
        me.CustomToggle("Target Wounded", &me.AimCfg.TargetWounded);

        ImGui::NextColumn();

        me.SectionHeader("AUTOMATION");
        me.CustomToggle("Auto Shoot", &me.AimCfg.AutoShoot);

        me.SectionHeader("BALLISTICS");
        ImGui::Text("Perfect Predict");
        me.CustomSliderFloat("Gravity Scale", &me.AimCfg.GravityScale, 0.0f, 3.0f, "%.2f");

        ImGui::Columns(1);
        Menu::PopLockZone();
        break;
    }
    case Tab::AntiAim: {
        Menu::PushLockZone(true);
        me.RenderAntiAimTab();
        Menu::PopLockZone();
        break;
    }
    case Tab::Weapon: {
        Menu::PushLockZone(true);
        ImGui::Columns(2, "WeaponCols", false);
        ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.55f);

        me.SectionHeader("WEAPON MODS");
        me.CustomToggle("No Recoil", &me.WeaponCfg.NoRecoil);
        me.CustomToggle("No Sway", &me.WeaponCfg.NoSway);
        me.CustomToggle("No Spread", &me.WeaponCfg.NoSpread);

        ImGui::NextColumn();

        me.SectionHeader("EXPLOITS");
        me.CustomToggle("Fast Bullet",  &me.WeaponCfg.FastBullet);
        if (me.WeaponCfg.FastBullet) {
            me.CustomSliderFloat("Bullet Speed", &me.WeaponCfg.FastBulletSpeed, 1.0f, 1.45f, "x%.2f");
        }
        me.CustomToggle("Thick Bullet", &me.WeaponCfg.ThickBullet);
        if (me.WeaponCfg.ThickBullet) {
            me.CustomSliderFloat("Thickness", &me.WeaponCfg.ThickBulletSize, 0.05f, 1.5f, "%.2fm");
        }
        me.CustomToggle("Double Tap",   &me.WeaponCfg.DoubleTap);
        me.CustomToggle("Manipulator",  &me.WeaponCfg.Manipulator);
        if (me.WeaponCfg.Manipulator) {
            me.CustomSliderFloat("Peek Distance", &me.WeaponCfg.ManipPeekDist, 0.3f, 1.5f, "%.2fm");
            KeybindWidget("Auto Peek Key",     &me.WeaponCfg.ManipAutoKey);
            KeybindWidget("Vertical Peek Key", &me.WeaponCfg.ManipVerticalKey);
        }
        me.CustomToggle("Wallshot", &me.WeaponCfg.AutoParent,
            "How to activate:\n\n"
            "1) Sit in / mount any vehicle or parent entity - modular car,\n"
            "   minicopter, scrap heli, horse, snowmobile, scooter, boat,\n"
            "   sub, hot-air balloon, tugboat, elevator platform.\n"
            "2) Stay seated / parented while shooting.\n"
            "3) Enable Manipulator + hold its peek key to shoot through walls.\n\n"
            "You MUST be parented to a vehicle for wallbangs to work -\n"
            "standing on the ground will not trigger this.");

        ImGui::Columns(1);
        Menu::PopLockZone();
        break;
    }
    case Tab::Visual: {
        me.RenderVisualsTab();
        break;
    }
    case Tab::World: {
        Menu::PushLockZone(true);
        ImGui::Columns(2, "WorldCols", false);
        ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.5f);

        me.SectionHeader("ENVIRONMENT");
        me.CustomToggle("Time Changer", &me.WorldCfg.TimeChanger);
        if (me.WorldCfg.TimeChanger) {
            me.CustomSliderFloat("Time of Day", &me.WorldCfg.TimeOfDay, 0.0f, 24.0f, "%.1fh");
        }
        me.CustomToggle("No Fog", &me.WorldCfg.NoFog);
        me.CustomToggle("Fog Color", &me.WorldCfg.SkyChanger);
        if (me.WorldCfg.SkyChanger) {
            me.CustomColorEdit("Fog Tint", me.WorldCfg.SkyColor);
        }
        me.CustomToggle("Sky Color", &me.WorldCfg.SkyColorChanger);
        if (me.WorldCfg.SkyColorChanger) {
            me.CustomColorEdit("Sky Tint", me.WorldCfg.SkyColorTint);
        }
        me.CustomToggle("Ambient Light", &me.WorldCfg.AmbientChanger);
        if (me.WorldCfg.AmbientChanger) {
            me.CustomColorEdit("Ambient Tint", me.WorldCfg.AmbientColor);
        }

        ImGui::NextColumn();

        me.SectionHeader("CAMERA");
        me.CustomToggle("FOV Changer", &me.WorldCfg.FOVChanger);
        if (me.WorldCfg.FOVChanger) {
            me.CustomSliderFloat("World FOV", &me.WorldCfg.CameraFOV, 50.0f, 130.0f, "%.0f");
        }

        me.SectionHeader("EFFECTS");
        me.CustomToggle("Night Mode", &me.WorldCfg.NightMode);
        if (me.WorldCfg.NightMode) {
            me.CustomSliderFloat("Darkness", &me.WorldCfg.NightModeAmount, 0.0f, 0.95f, "%.2f");
        }
        me.CustomToggle("Bullet Tracer", &me.WorldCfg.BulletTracer);
        if (me.WorldCfg.BulletTracer) {
            ImGui::Indent(14.0f);
            me.CustomToggle("Rainbow", &me.WorldCfg.TracerRainbow);
            if (!me.WorldCfg.TracerRainbow) {
                me.CustomColorEdit("Tracer Color", me.WorldCfg.TracerColor);
            }
            me.CustomSliderFloat("Thickness", &me.WorldCfg.TracerThickness, 0.5f, 5.0f, "%.1f px");
            me.CustomSliderFloat("Lifetime",  &me.WorldCfg.TracerLife, 0.5f, 10.0f, "%.1f s");
            ImGui::Unindent(14.0f);
        }

        ImGui::Columns(1);
        Menu::PopLockZone();
        break;
    }
    case Tab::Misc: {
        Menu::PushLockZone(true);
        ImGui::Columns(2, "MiscCols", false);
        ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.5f);

        me.SectionHeader("MOVEMENT");
        me.CustomToggle("Flyhack", &me.MiscCfg.Flyhack);
        if (me.MiscCfg.Flyhack) {
            me.CustomSliderFloat("Fly Speed", &me.MiscCfg.FlySpeed, 1.0f, 60.0f, "%.1f");
        }
        me.CustomToggle("Spider", &me.MiscCfg.Spider);
        me.CustomToggle("No Fall Damage", &me.MiscCfg.NoFallDmg);
        me.CustomToggle("Speedhack", &me.MiscCfg.Speedhack);
        if (me.MiscCfg.Speedhack) {
            me.CustomSliderFloat("Speed Scale", &me.MiscCfg.SpeedScale, 0.25f, 4.0f, "%.2fx");
        }

        ImGui::NextColumn();

        me.SectionHeader("WORLD EXPLOITS");
        me.CustomToggle("No Gravity", &me.MiscCfg.NoGravity);
        me.CustomToggle("Long Render", &me.MiscCfg.LongRender);
        if (me.MiscCfg.LongRender) {
            me.CustomSliderFloat("Render Dist", &me.MiscCfg.RenderDist, 2000.0f, 50000.0f, "%.0f m");
        }

        me.SectionHeader("COMBAT EXPLOITS");
        me.CustomToggle("Insta Attack", &me.MiscCfg.InstaAttack);
        me.CustomToggle("Full Auto",    &me.MiscCfg.FullAuto);
        me.CustomToggle("Silent Shots", &me.MiscCfg.SilentShots);
        me.CustomToggle("Super Eoka",   &me.MiscCfg.SuperEoka);
        me.CustomToggle("Fast Bow",     &me.MiscCfg.FastBow);
        me.CustomToggle("Jump Shoot",   &me.MiscCfg.JumpShoot);
        me.CustomToggle("Melee Reach",  &me.MiscCfg.MeleeReach);
        if (me.MiscCfg.MeleeReach) {
            me.CustomSliderFloat("Reach Mul", &me.MiscCfg.MeleeReachV, 1.0f, 20.0f, "%.2fx");
        }
        me.CustomToggle("Fake Admin",   &me.MiscCfg.FakeAdmin);

        me.SectionHeader("DOS / GRIEF");
        me.CustomToggle("RPC DoS", &me.MiscCfg.RpcDos,
            "Spams ConsoleSystem.SendToServer with garbage console commands "
            "while the toggle is on. Each call goes through "
            "ConsoleNetwork.OnClientCommand and ConsoleSystem.RunWithResult on "
            "the server. At ~200 calls/sec it ties up the server's command "
            "parser and serialisation thread.\n\n"
            "WARNING: heavy server-side log footprint, instant ulog/RCon trail. "
            "Use only on servers you don't care about.");
        if (me.MiscCfg.RpcDos) {
            me.CustomSliderInt("RPC DoS Rate", &me.MiscCfg.RpcDosRate, 10, 100000);
        }
        me.CustomToggle("JSON DoS", &me.MiscCfg.JsonDos,
            "Sends huge nested-array console-command strings (up to 8 MB) to "
            "the server while the toggle is on. Server allocates a StringRaw "
            "buffer of that size and runs ConsoleSystem command lookup on the "
            "garbage. Forces server GC pressure and main-thread stalls.\n\n"
            "Size slider sets bytes per packet. >256KB triggers the "
            "maxpacketsize_command drop on most vanilla servers; smaller sizes "
            "(64KB) sneak under the limit and actually execute.");
        if (me.MiscCfg.JsonDos) {
            me.CustomSliderInt("JSON DoS Size", &me.MiscCfg.JsonDosSize, 4096, 100048576);
        }

        ImGui::Columns(1);
        Menu::PopLockZone();
        break;
    }
    case Tab::Settings: {
        ImGui::Columns(2, "SettingsCols", false);
        ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.5f);

        me.SectionHeader("BACKGROUND PARTICLES");
        const char* pModes[] = { "Disabled", "Stars", "Snowfall", "Vortex", "Nexus" };
        me.CustomCombo("Effect Type", &me.ParticleCfg.Mode, pModes, 5);

        me.SectionHeader("SYSTEM");
        me.CustomToggle("Show Watermark", &me.MiscCfg.Watermark);
        me.CustomToggle("Show Keybinds",  &me.SettingsCfg.ShowKeybinds);
        me.CustomToggle("Save On Exit",   &me.SettingsCfg.SaveOnExit);

        ImGui::NextColumn();

        me.SectionHeader("CONFIG");
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputText("##cfgname", me.m_cfgNameBuf, sizeof(me.m_cfgNameBuf),
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::PopItemWidth();

        float btnW3 = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
        if (ImGui::Button("Save", ImVec2(btnW3, 24)))   g_lockRequest = true;
        ImGui::SameLine();
        if (ImGui::Button("Load", ImVec2(btnW3, 24)))   g_lockRequest = true;
        ImGui::SameLine();
        if (ImGui::Button("Delete", ImVec2(btnW3, 24))) g_lockRequest = true;

        ImGui::Spacing();
        {
            float listH = 60.0f;
            ImGui::BeginChild("##cfglist", ImVec2(0, listH), true);
            ImGui::TextColored(ImVec4(0.65f, 0.45f, 0.45f, 1.0f),
                               "Config save/load is paid feature.");
            ImGui::EndChild();
        }

        ImGui::Columns(1);
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 65); 
        if (ImGui::Button("Unload", ImVec2(ImGui::GetContentRegionAvail().x, 30))) exit(0);
        break;
    }
    default: break;
    }

    ImGui::PopStyleVar(); 
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(); 
}

void Menu::Render() {
    auto& me = Get();

    auto now = std::chrono::steady_clock::now();
    me.m_deltaTime = std::chrono::duration<float>(now - me.m_lastFrame).count();
    me.m_deltaTime = (std::max)(1.0f / 240.0f, (std::min)(me.m_deltaTime, 0.05f));
    me.m_lastFrame = now;

    me.m_contentAlpha = me.Lerp(me.m_contentAlpha, 1.0f, me.m_deltaTime * 8.0f);
    me.m_openAnim = me.Lerp(me.m_openAnim, me.m_isOpen ? 1.0f : 0.0f, me.m_deltaTime * 10.0f);

    if (!me.m_isOpen && me.m_openAnim < 0.01f) {
        me.m_openAnim = 0.0f;
        return;
    }
    if (me.m_isOpen && me.m_openAnim > 0.999f) {
        me.m_openAnim = 1.0f;
    }

    me.RenderBackgroundParticles();

    const ImVec2 kMenuSize(720.0f, 500.0f);
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (disp.x > 0.0f && disp.y > 0.0f) {
        ImGui::SetNextWindowPos(
            ImVec2((disp.x - kMenuSize.x) * 0.5f, (disp.y - kMenuSize.y) * 0.5f),
            ImGuiCond_Appearing);
    }
    ImGui::SetNextWindowSize(kMenuSize);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, me.m_openAnim * me.SettingsCfg.MenuAlpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGui::Begin("##RainMenu", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wPos = ImGui::GetWindowPos();
        ImVec2 wSize = ImGui::GetWindowSize();

        dl->AddRect(wPos, ImVec2(wPos.x + wSize.x, wPos.y + wSize.y),
            me.GetAccentColorU32(0.25f), 10.0f, 0, 1.5f);

        me.RenderTabBar(dl, wPos, ImVec2(wSize.x, TAB_BAR_HEIGHT));

        ImGui::SetCursorPos(ImVec2(0, TAB_BAR_HEIGHT + 2));
        me.RenderContent();

        float footerY = wPos.y + wSize.y - 22;
        dl->AddLine(ImVec2(wPos.x + 10, footerY), ImVec2(wPos.x + wSize.x - 10, footerY),
            IM_COL32(30, 30, 38, 255));
            
        float pulseFast = (sinf((float)ImGui::GetTime() * 3.0f) + 1.0f) * 0.5f;

        ImVec4 baseCol = ImVec4(0.35f, 0.35f, 0.40f, 1.00f);
        ImVec4 accentCol = me.GetAccentColor();
        ImU32 rainCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
            Lerp(baseCol.x, accentCol.x, pulseFast),
            Lerp(baseCol.y, accentCol.y, pulseFast),
            Lerp(baseCol.z, accentCol.z, pulseFast), 1.0f));
            
        dl->AddText(ImVec2(wPos.x + 14, footerY + 4), rainCol, "Rainhack | ALKAD");

        float pulse = (sinf((float)ImGui::GetTime() * 2.0f) + 1.0f) * 0.5f;
        
        float dotX = wPos.x + wSize.x - 24;
        dl->AddCircleFilled(ImVec2(dotX, footerY + 10), 4.0f,
            me.GetAccentColorU32(0.4f + pulse * 0.5f), 12);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    if (g_lockRequest) {
        ImGui::OpenPopup("##FreeBuyPopup");
        g_lockRequest = false;
    }
    {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_PopupBg,    ImVec4(0.06f, 0.06f, 0.09f, 0.97f));
        ImGui::PushStyleColor(ImGuiCol_Border,     ImVec4(0.85f, 0.20f, 0.25f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_Button,     ImVec4(0.18f, 0.07f, 0.09f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.10f, 0.13f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.40f, 0.14f, 0.18f, 1.00f));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 12.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 18));

        if (ImGui::BeginPopupModal("##FreeBuyPopup", nullptr,
                                   ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove)) {
            float w = ImGui::GetContentRegionAvail().x;

            const char* title = "Go buy paid broo";
            ImVec2 ts = ImGui::CalcTextSize(title);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - ts.x * 1.6f) * 0.5f);
            ImGui::SetWindowFontScale(1.6f);
            ImGui::TextColored(ImVec4(1.00f, 0.45f, 0.50f, 1.00f), "%s", title);
            ImGui::SetWindowFontScale(1.0f);

            ImGui::Dummy(ImVec2(0, 6));
            const char* sub = "This feature is locked in the FREE build.";
            ImVec2 ss = ImGui::CalcTextSize(sub);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - ss.x) * 0.5f);
            ImGui::TextColored(ImVec4(0.78f, 0.78f, 0.85f, 1.00f), "%s", sub);

            const char* sub2 = "Upgrade to PAID to unlock the full menu.";
            ImVec2 ss2 = ImGui::CalcTextSize(sub2);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - ss2.x) * 0.5f);
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.00f), "%s", sub2);

            ImGui::Dummy(ImVec2(0, 14));
            float btnW = 180.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - btnW) * 0.5f);
            if (ImGui::Button("OK", ImVec2(btnW, 32))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(5);
    }
}

void Menu::RenderWatermark() {
    auto& me = Get();
    if (!me.MiscCfg.Watermark) return;

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList(); 
    
    std::string text = "Rainhack | Stable | " + std::to_string((int)io.Framerate) + " FPS";
    ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    
    float paddingX = 14.0f;
    float paddingY = 8.0f;
    ImVec2 boxSize(textSize.x + paddingX * 2, textSize.y + paddingY * 2);
    
    ImVec2 pos(io.DisplaySize.x - boxSize.x - 20, 20);
    
    dl->AddRectFilled(pos, ImVec2(pos.x + boxSize.x, pos.y + boxSize.y), 
        IM_COL32(10, 10, 15, 220), 6.0f);
    
    dl->AddRect(pos, ImVec2(pos.x + boxSize.x, pos.y + boxSize.y),
        me.GetAccentColorU32(0.3f), 6.0f, 0, 1.5f);

    dl->AddRectFilled(pos, ImVec2(pos.x + 3, pos.y + boxSize.y),
        me.GetAccentColorU32(0.9f), 6.0f, ImDrawFlags_RoundCornersLeft);

    dl->AddText(ImVec2(pos.x + paddingX, pos.y + paddingY), 
        IM_COL32(230, 235, 245, 255), text.c_str());
}

void Menu::RenderFreeWatermark() {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    ImFont* font = ImGui::GetFont();
    if (!font) return;

    const char* text = "RAINHACK - FREE";
    int n = (int)strlen(text);

    float baseFs = ImGui::GetFontSize();
    if (baseFs < 14.0f) baseFs = 14.0f;
    float fontSize = baseFs * 2.1f;
    if (fontSize < 30.0f) fontSize = 30.0f;

    ImVec2 totalSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);

    float screenPad = 16.0f;
    float boxPadX   = 12.0f;
    float boxPadY   = 7.0f;
    ImVec2 boxMin(io.DisplaySize.x - totalSize.x - boxPadX * 2.0f - screenPad, screenPad);
    ImVec2 boxMax(boxMin.x + totalSize.x + boxPadX * 2.0f,
                  boxMin.y + totalSize.y + boxPadY * 2.0f);

    dl->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 150), 12.0f);
    dl->AddRectFilled(ImVec2(boxMin.x + 2, boxMin.y + 2),
                      ImVec2(boxMax.x - 2, boxMax.y - 2),
                      IM_COL32(8, 8, 14, 180), 11.0f);

    float t = (float)ImGui::GetTime();

    ImVec2 cursor(boxMin.x + boxPadX, boxMin.y + boxPadY);
    for (int i = 0; i < n; ++i) {
        char ch = text[i];
        char single[2] = { ch, 0 };
        ImVec2 cs = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, single);

        if (ch != ' ') {
            float hue = fmodf(t * 0.35f + (float)i * 0.075f, 1.0f);
            float r, g, b;
            ImGui::ColorConvertHSVtoRGB(hue, 0.85f, 1.0f, r, g, b);

            ImU32 textCol = IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 250);
            ImU32 shadowCol = IM_COL32(0, 0, 0, 210);
            ImU32 glowCol = IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 70);

            dl->AddText(font, fontSize, ImVec2(cursor.x + 2, cursor.y + 2), shadowCol, single);
            dl->AddText(font, fontSize, ImVec2(cursor.x - 1, cursor.y),     glowCol,   single);
            dl->AddText(font, fontSize, ImVec2(cursor.x + 1, cursor.y),     glowCol,   single);
            dl->AddText(font, fontSize, cursor, textCol, single);
        }
        cursor.x += cs.x;
    }

    {
        float hue = fmodf(t * 0.22f, 1.0f);
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(hue, 0.80f, 1.0f, r, g, b);
        ImU32 borderCol = IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 230);
        dl->AddRect(boxMin, boxMax, borderCol, 12.0f, 0, 2.5f);
    }

    {
        float pulse = (sinf(t * 4.0f) * 0.5f + 0.5f);
        float bandH = 3.0f;
        ImVec2 bandMin(boxMin.x + 8.0f,  boxMax.y - bandH - 4.0f);
        ImVec2 bandMax(boxMax.x - 8.0f,  boxMax.y - 4.0f);
        float w = bandMax.x - bandMin.x;
        int segs = 24;
        for (int i = 0; i < segs; ++i) {
            float x0 = bandMin.x + (w * (float)i) / segs;
            float x1 = bandMin.x + (w * (float)(i + 1)) / segs;
            float hue = fmodf(t * 0.6f + (float)i / (float)segs, 1.0f);
            float r, g, b;
            ImGui::ColorConvertHSVtoRGB(hue, 0.9f, 0.6f + 0.4f * pulse, r, g, b);
            dl->AddRectFilled(ImVec2(x0, bandMin.y), ImVec2(x1, bandMax.y),
                              IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 210), 0.0f);
        }
    }
}

void Menu::RenderBackgroundParticles() {
    auto& me = Get();
    if (me.ParticleCfg.Mode == 0) return;

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    if (me.m_particles.size() != (size_t)me.ParticleCfg.Count) {
        me.m_particles.resize(me.ParticleCfg.Count);
        for (auto& p : me.m_particles) {
            if (p.size < 0.1f) { 
                p.pos = ImVec2((float)(rand() % (int)io.DisplaySize.x), (float)(rand() % (int)io.DisplaySize.y));
                p.size = (float)(rand() % 20 + 10) * 0.1f;
                p.vel = ImVec2((float)(rand() % 40 - 20) * 0.05f * (p.size * 0.5f), (float)(rand() % 40 - 20) * 0.05f * (p.size * 0.5f));
                p.randomSeed = (float)(rand() % 1000) * 0.01f;
            }
        }
    }

    float animFactor = me.m_openAnim;
    if (animFactor < 0.001f) return;

    float time = (float)ImGui::GetTime();
    float dt = me.m_deltaTime * 1.0f * 12.0f;

    ImVec2 mousePos = io.MousePos;
    float repulsionRadius = 120.0f;
    float repulsionStrength = 3.5f;

    for (auto& p : me.m_particles) {
        float dx = p.pos.x - mousePos.x;
        float dy = p.pos.y - mousePos.y;
        float distSq = dx * dx + dy * dy;
        
        if (distSq < repulsionRadius * repulsionRadius) {
            float dist = sqrtf(distSq);
            if (dist < 1.0f) dist = 1.0f;
            float force = (repulsionRadius - dist) / repulsionRadius;
            p.pos.x += (dx / dist) * force * repulsionStrength;
            p.pos.y += (dy / dist) * force * repulsionStrength;
        }

        if (me.ParticleCfg.Mode == 1 || me.ParticleCfg.Mode == 4) { 
            p.pos.x += p.vel.x * dt;
            p.pos.y += p.vel.y * dt;
        } 
        else if (me.ParticleCfg.Mode == 2) { 
            p.pos.y += (p.size * 2.5f + 1.0f) * dt * 2.0f;
            p.pos.x += sinf(time + p.randomSeed) * dt;
        }
        else if (me.ParticleCfg.Mode == 3) { 
            ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
            float vx = p.pos.x - center.x;
            float vy = p.pos.y - center.y;
            float dist = sqrtf(vx*vx + vy*vy);
            float angle = atan2f(vy, vx);
            angle += (110.0f / (dist + 60.0f)) * dt * 0.2f;
            p.pos.x = center.x + cosf(angle) * dist;
            p.pos.y = center.y + sinf(angle) * dist;
        }

        if (p.pos.x < -30) p.pos.x = io.DisplaySize.x + 30;
        if (p.pos.x > io.DisplaySize.x + 30) p.pos.x = -30;
        if (p.pos.y < -30) p.pos.y = io.DisplaySize.y + 30;
        if (p.pos.y > io.DisplaySize.y + 30) p.pos.y = -30;
    }

    if (me.ParticleCfg.Mode == 4) {
        float maxDistSq = 120.0f * 120.0f;
        for (size_t i = 0; i < me.m_particles.size(); i++) {
            for (size_t j = i + 1; j < me.m_particles.size(); j++) {
                float dx = me.m_particles[i].pos.x - me.m_particles[j].pos.x;
                float dy = me.m_particles[i].pos.y - me.m_particles[j].pos.y;
                float dSq = dx * dx + dy * dy;

                if (dSq < maxDistSq) {
                    float dist = sqrtf(dSq);
                    float alpha = (1.0f - (dist / 120.0f)) * 0.45f * animFactor;
                    
                    ImU32 linkCol = me.GetAccentColorU32(alpha);
                    
                    dl->AddLine(me.m_particles[i].pos, me.m_particles[j].pos, linkCol, 1.2f);
                }
            }
        }
    }

    for (auto& p : me.m_particles) {
        float flicker = (sinf(time * 1.5f + p.randomSeed) + 1.0f) * 0.5f;
        float currentAlpha = (0.3f + 0.7f * flicker) * animFactor;
        
        ImVec4 accent = me.GetAccentColor();
        ImU32 col = me.GetAccentColorU32(currentAlpha);
        
        dl->AddCircleFilled(p.pos, p.size * 2.0f, me.GetAccentColorU32(0.12f * currentAlpha), 12);
        dl->AddCircleFilled(p.pos, p.size, col, 12);
        
        if (me.ParticleCfg.Mode == 4 && p.size > 1.8f) { 
             dl->AddCircleFilled(p.pos, p.size * 0.5f, IM_COL32(255, 255, 255, (int)(200 * currentAlpha)), 12);
        }
    }
}

void Menu::RenderAntiAimTab() {
    auto& me = Get();
    auto& aa = me.AntiAimCfg;

    ImGui::Columns(2, "AntiAimCols", false);
    ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.5f);

    me.SectionHeader("GENERAL");
    me.CustomToggle("Enable Anti-Aim", &aa.Enable);

    if (!aa.Enable) {
        ImGui::Columns(1);
        return;
    }

    me.CustomToggle("Disable While Aiming", &aa.DisableOnAim);
    me.CustomToggle("Only When Moving", &aa.OnlyMoving);
    me.CustomToggle("Only When Standing", &aa.OnlyStanding);

    me.SectionHeader("YAW");
    const char* yawModes[] = { "Off", "Backward", "Sideways", "Jitter", "Spin", "Random" };
    me.CustomCombo("Yaw Mode", &aa.YawMode, yawModes, 6);
    if (aa.YawMode != 0) {
        me.CustomSliderFloat("Base Offset",  &aa.YawBase,    -180.0f, 180.0f, "%.0f deg");
    }
    if (aa.YawMode == 3 || aa.YawMode == 5) {
        me.CustomSliderFloat("Jitter Range", &aa.YawJitter,  0.0f,    180.0f, "%.0f deg");
    }
    if (aa.YawMode == 4) {
        me.CustomSliderFloat("Spin Speed",   &aa.SpinSpeed,  30.0f,   900.0f, "%.0f d/s");
    }

    ImGui::NextColumn();

    me.SectionHeader("PITCH");
    const char* pitchModes[] = { "Off", "Down", "Up", "Zero", "Jitter", "Random" };
    me.CustomCombo("Pitch Mode", &aa.PitchMode, pitchModes, 6);
    if (aa.PitchMode == 1 || aa.PitchMode == 2) {
        me.CustomSliderFloat("Pitch Angle",  &aa.PitchValue,  0.0f, 89.0f, "%.0f deg");
    }
    if (aa.PitchMode == 4 || aa.PitchMode == 5) {
        me.CustomSliderFloat("Pitch Jitter", &aa.PitchJitter, 0.0f, 89.0f, "%.0f deg");
    }

    me.SectionHeader("ROLL / LEAN");
    me.CustomToggle("Enable Roll", &aa.EnableRoll);
    if (aa.EnableRoll) {
        const char* rollModes[] = { "Static", "Wave", "Jitter" };
        me.CustomCombo("Roll Mode", &aa.RollMode, rollModes, 3);
        me.CustomSliderFloat("Roll Angle", &aa.RollValue, 0.0f, 89.0f, "%.0f deg");
        if (aa.RollMode != 0) {
            me.CustomSliderFloat("Roll Speed", &aa.RollJitterSpd, 0.5f, 30.0f, "%.1f Hz");
        }
    }

    me.SectionHeader("DESYNC");
    me.CustomToggle("Fake Desync", &aa.FakeDesync);
    if (aa.FakeDesync) {
        me.CustomSliderFloat("Desync Amount", &aa.DesyncAmount, 5.0f, 90.0f, "%.0f deg");
    }

    me.SectionHeader("DISPLAY");
    me.CustomToggle("Show Indicator", &aa.ShowIndicator);

    ImGui::Columns(1);
}

void Menu::RenderAntiAimIndicator() {
    auto& me = Get();
    auto& aa = me.AntiAimCfg;
    if (!aa.Enable || !aa.ShowIndicator) return;

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    const char* yawModes[]   = { "OFF", "BACKWARD", "SIDEWAYS", "JITTER", "SPIN", "RANDOM" };
    const char* pitchModes[] = { "OFF", "DOWN", "UP", "ZERO", "JITTER", "RANDOM" };

    float baseX = 20.0f;
    float baseY = io.DisplaySize.y - 140.0f;

    const char* statusLabel = "ANTI-AIM";
    ImVec2 lblSize = ImGui::CalcTextSize(statusLabel);

    ImVec2 boxPos(baseX, baseY);
    ImVec2 boxSize(170.0f, 96.0f);

    dl->AddRectFilled(boxPos,
                      ImVec2(boxPos.x + boxSize.x, boxPos.y + boxSize.y),
                      IM_COL32(10, 10, 15, 200), 6.0f);
    dl->AddRect(boxPos,
                ImVec2(boxPos.x + boxSize.x, boxPos.y + boxSize.y),
                me.GetAccentColorU32(0.55f), 6.0f, 0, 1.4f);
    dl->AddRectFilled(boxPos,
                      ImVec2(boxPos.x + 3, boxPos.y + boxSize.y),
                      me.GetAccentColorU32(0.95f), 6.0f, ImDrawFlags_RoundCornersLeft);

    float pulse = (sinf((float)ImGui::GetTime() * 4.0f) + 1.0f) * 0.5f;
    ImU32 titleCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
        Lerp(0.85f, 1.00f, pulse),
        Lerp(0.25f, 0.60f, pulse),
        Lerp(0.95f, 1.00f, pulse), 1.0f));
    dl->AddText(ImVec2(boxPos.x + 12, boxPos.y + 8), titleCol, statusLabel);

    bool inverted = aa.InverterKey != 0
                    && (GetAsyncKeyState(aa.InverterKey) & 0x8000) != 0;
    inverted ^= aa.InverterState;

    char line[96];
    float ty = boxPos.y + 8 + lblSize.y + 6;
    float stepY = 15.0f;

    snprintf(line, sizeof(line), "YAW : %s%s",
             yawModes[(std::min)(aa.YawMode, 5)],
             inverted ? " <-" : " ->");
    dl->AddText(ImVec2(boxPos.x + 12, ty),
                IM_COL32(220, 225, 235, 255), line);
    ty += stepY;

    snprintf(line, sizeof(line), "PITCH : %s", pitchModes[(std::min)(aa.PitchMode, 5)]);
    dl->AddText(ImVec2(boxPos.x + 12, ty),
                IM_COL32(220, 225, 235, 255), line);
    ty += stepY;

    if (aa.EnableRoll) {
        snprintf(line, sizeof(line), "ROLL : %.0f deg", aa.RollValue);
        dl->AddText(ImVec2(boxPos.x + 12, ty),
                    IM_COL32(200, 180, 255, 255), line);
        ty += stepY;
    }

    if (aa.FakeDesync) {
        snprintf(line, sizeof(line), "DSYNC : %.0f deg", aa.DesyncAmount);
        dl->AddText(ImVec2(boxPos.x + 12, ty),
                    IM_COL32(160, 200, 255, 255), line);
    }
}

void Menu::RenderKeybindsList() {
    auto& me = Get();
    ImGuiIO& io = ImGui::GetIO();
    float dt = io.DeltaTime;
    if (dt <= 0.0f || dt > 0.1f) dt = 1.0f / 60.0f;

    static float s_masterAlpha = 0.0f;
    float masterTarget = me.SettingsCfg.ShowKeybinds ? 1.0f : 0.0f;
    s_masterAlpha = Lerp(s_masterAlpha, masterTarget, dt * 8.0f);
    if (s_masterAlpha < 0.01f) return;

    struct Entry {
        const char* label;
        int vk;
        bool feature;
    };

    Entry entries[] = {
        { "Menu",          VK_INSERT,                       true },
        { "Aim Bot",       VK_RBUTTON,                      me.AimCfg.Enabled && !me.AimCfg.PerfectSilent },
        { "Silent Aim",    VK_LBUTTON,                      me.AimCfg.PerfectSilent },
        { "Auto Peek",     me.WeaponCfg.ManipAutoKey,       me.WeaponCfg.Manipulator },
        { "Vertical Peek", me.WeaponCfg.ManipVerticalKey,   me.WeaponCfg.Manipulator },
        { "RPC DoS",       me.MiscCfg.RpcDosKey,            me.MiscCfg.RpcDos },
        { "JSON DoS",      me.MiscCfg.JsonDosKey,           me.MiscCfg.JsonDos },
    };
    const int kEntryCount = sizeof(entries) / sizeof(entries[0]);

    static std::unordered_map<std::string, float> s_entryAlpha;

    int visibleCount = 0;
    float entryAlpha[kEntryCount] = {};
    for (int i = 0; i < kEntryCount; ++i) {
        const Entry& e = entries[i];
        bool held = e.vk != 0
                    && (GetAsyncKeyState(e.vk) & 0x8000) != 0
                    && e.feature;
        float& a = s_entryAlpha[e.label];
        a = Lerp(a, held ? 1.0f : 0.0f, dt * 12.0f);
        if (a < 0.01f) a = 0.0f;
        entryAlpha[i] = a;
        if (a > 0.01f) visibleCount++;
    }

    static float s_panelAlpha = 0.0f;
    float panelTarget = (visibleCount > 0) ? 1.0f : 0.0f;
    s_panelAlpha = Lerp(s_panelAlpha, panelTarget, dt * 8.0f);
    if (s_panelAlpha < 0.01f) return;

    float combinedAlpha = s_masterAlpha * s_panelAlpha;

    const float kHeaderH  = 26.0f;
    const float kEntryH   = 22.0f;
    const float kPadX     = 12.0f;
    const float kPadY     = 8.0f;
    const float kPanelW   = 200.0f;

    float panelH = kHeaderH + kPadY * 2;
    for (int i = 0; i < kEntryCount; ++i) panelH += entryAlpha[i] * kEntryH;

    ImVec2 panelPos(io.DisplaySize.x - kPanelW - 16.0f, 70.0f);
    ImVec2 panelEnd(panelPos.x + kPanelW, panelPos.y + panelH);

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    ImU32 bgCol     = IM_COL32(10, 10, 15, (int)(210 * combinedAlpha));
    ImU32 borderCol = me.GetAccentColorU32(0.55f * combinedAlpha);
    ImU32 stripCol  = me.GetAccentColorU32(0.95f * combinedAlpha);

    dl->AddRectFilled(panelPos, panelEnd, bgCol, 6.0f);
    dl->AddRect      (panelPos, panelEnd, borderCol, 6.0f, 0, 1.4f);
    dl->AddRectFilled(panelPos, ImVec2(panelPos.x + 3.0f, panelEnd.y),
                      stripCol, 6.0f, ImDrawFlags_RoundCornersLeft);

    const char* title = "KEYBINDS";
    ImVec2 ts = ImGui::CalcTextSize(title);
    ImU32 titleCol = ImGui::ColorConvertFloat4ToU32(
        ImVec4(0.95f, 0.40f, 0.95f, 1.0f * combinedAlpha));
    dl->AddText(ImVec2(panelPos.x + kPadX, panelPos.y + 6.0f), titleCol, title);

    float sepY = panelPos.y + kHeaderH;
    dl->AddLine(ImVec2(panelPos.x + 6.0f, sepY),
                ImVec2(panelEnd.x  - 6.0f, sepY),
                me.GetAccentColorU32(0.25f * combinedAlpha), 1.0f);

    float yCursor = sepY + kPadY;

    for (int i = 0; i < kEntryCount; ++i) {
        if (entryAlpha[i] < 0.01f) continue;
        const Entry& e = entries[i];
        float a = entryAlpha[i] * combinedAlpha;

        const char* keyName = VkToName(e.vk);
        ImVec2 keyTs = ImGui::CalcTextSize(keyName);

        float dimR = 0.55f, dimG = 0.58f, dimB = 0.65f;
        float onR  = 1.0f,  onG  = 1.0f,  onB  = 1.0f;
        ImU32 lblCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
            Lerp(dimR, onR, entryAlpha[i]),
            Lerp(dimG, onG, entryAlpha[i]),
            Lerp(dimB, onB, entryAlpha[i]),
            a));
        dl->AddText(ImVec2(panelPos.x + kPadX, yCursor + 2.0f), lblCol, e.label);

        float keyBoxW = keyTs.x + 14.0f;
        float keyBoxH = kEntryH - 6.0f;
        ImVec2 keyBoxMin(panelEnd.x - kPadX - keyBoxW, yCursor + 1.0f);
        ImVec2 keyBoxMax(keyBoxMin.x + keyBoxW, keyBoxMin.y + keyBoxH);

        ImU32 keyBoxCol = me.GetAccentColorU32(0.85f * a);
        ImU32 keyTextCol = IM_COL32(255, 255, 255, (int)(255 * a));
        dl->AddRectFilled(keyBoxMin, keyBoxMax, keyBoxCol, 4.0f);
        dl->AddText(ImVec2(keyBoxMin.x + (keyBoxW - keyTs.x) * 0.5f,
                           keyBoxMin.y + (keyBoxH - keyTs.y) * 0.5f),
                    keyTextCol, keyName);

        yCursor += entryAlpha[i] * kEntryH;
    }
}
