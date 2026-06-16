#pragma once
#include "../../ext/imgui/imgui.h"
#include "../../ext/imgui/imgui_internal.h"
#include <string>
#include <chrono>
#include <unordered_map>
#include <vector>

class Menu {
public:
    static void Initialize();
    static void Render();
    static void RenderWatermark();
    static void RenderAntiAimIndicator();
    static void RenderKeybindsList();
    static void RenderFreeWatermark();
    static void SetStyle();
    static void SetVisible(bool v);
    static bool IsVisible();

    static void PushLockZone(bool locked);
    static void PopLockZone();

    bool SaveConfig(const char* name);
    bool LoadConfig(const char* name);
    bool DeleteConfig(const char* name);
    std::vector<std::string> GetConfigList();
    static std::string GetConfigDir();

    struct AimConfig {
        bool Enabled = false;
        bool PerfectSilent = false;
        int Hitbox = 0;
        float FOV = 100.0f;
        bool ShowFOV = false;
        bool TargetNPC = false;
        bool TargetSleeping = false;
        bool TargetWounded = false;
        bool AutoShoot = false;
        // Ballistic prediction — compensate for bullet travel time & gravity.
        // Disable if the user explicitly wants "aim straight at head" (useful
        // for testing or for weapons where the gravity model is wrong).
        bool PredictMovement = true;   // lead moving targets by their velocity
        bool CompensateDrop  = true;   // aim above by 0.5*g*t^2
        // Per-shot gravityModifier + drag are ALWAYS auto-detected from the
        // live ammo prototype (`Projectile.gravityModifier` / `Projectile.drag`,
        // both public unobfuscated fields resolved via il2cpp). The previous
        // AutoGravity / ApplyDrag toggles were removed — they're forced on.
        // GravityScale below is a fine-tune multiplier on top of the
        // auto-detected gravity (default 1.0 = trust the live value;
        // push to 1.5 / 2.0 on modded servers with non-standard physics).
        float GravityScale   = 1.0f;
        bool VisibleCheck    = false;
    } AimCfg;

    struct WeaponConfig {
        bool NoRecoil = false;
        bool NoSway = false;
        bool NoSpread = false;
        bool FastBullet = false;
        float FastBulletSpeed = 1.4f;
        bool ThickBullet = false;
        float ThickBulletSize = 0.5f;
        bool DoubleTap = false;
        bool Manipulator = false;
        float ManipPeekDist = 0.7f;
        int ManipAutoKey = 0x06;
        int ManipVerticalKey = 0x05;
        bool AutoParent = false;
    } WeaponCfg;

    struct VisualConfig {
        int SelectedCategory = 0;

        struct PlayerESPSettings {
            bool Enabled       = false;
            int  BoxType       = 2;
            bool Name          = false;
            bool Distance      = false;
            bool TeamID        = false;
            bool Health        = false;
            bool Weapon        = false;
            bool ViewDirection = false;
            bool TargetLine    = false;
            bool TargetBelt    = false;
            bool OOFIndicator  = false;
            bool OutsideMark   = false;
            bool Skeleton      = false;
            float SkeletonThickness = 1.4f;

            bool DrawTeam        = true;
            bool DrawEnemies     = true;
            bool DrawWounded     = true;
            bool DrawDead        = false;
            bool DrawSleeping    = false;
            bool OnlineSleeperOnly = true;
            bool DrawSafezone    = true;

            bool  UseVisibleColor = true;
            float ColorEnemy[4]    = { 1.00f, 0.20f, 0.20f, 1.00f };
            float ColorTeam[4]     = { 0.20f, 0.85f, 1.00f, 1.00f };
            float ColorVisible[4]  = { 0.20f, 1.00f, 0.30f, 1.00f };
            float ColorSleeping[4] = { 0.55f, 0.55f, 0.62f, 1.00f };
            float ColorWounded[4]  = { 1.00f, 0.55f, 0.00f, 1.00f };
            float ColorDead[4]     = { 0.45f, 0.30f, 0.30f, 1.00f };
            float ColorOOF[4]      = { 1.00f, 0.30f, 0.85f, 1.00f };

            float TextSpacing  = 14.0f;
            float DrawDistance = 500.0f;
            float OOFRadius    = 220.0f;
        };

        struct NPCESPSettings {
            bool Enabled    = false;
            int  BoxType    = 2;
            bool Name       = false;
            bool Distance   = false;
            bool Health     = false;
            bool Weapon     = false;
            bool TargetLine = false;
            bool Skeleton   = false;
            float SkeletonThickness = 1.4f;
            float Color[4]  = { 1.00f, 0.85f, 0.20f, 1.00f };
            float TextSpacing  = 14.0f;
            float DrawDistance = 200.0f;
        };

        struct VehicleESPSettings {
            bool Enabled         = false;
            int  BoxType         = 1;
            bool Name            = true;
            bool Distance        = true;
            bool Health          = true;
            float DrawDistance   = 800.0f;
            float Color[4]       = { 1.00f, 0.95f, 0.40f, 1.00f };

            bool PatrolHeli      = true;
            bool Bradley         = true;
            bool Drones          = true;
            bool CargoShip       = true;
            bool Minicopter      = true;
            bool ScrapHeli       = true;
            bool Rowboat         = true;
            bool Submarine       = true;
            bool AttackHeli      = true;
            bool TugBoat         = true;
            bool Bikes           = true;
        };

        struct DeployableESPSettings {
            bool Enabled         = false;
            int  BoxType         = 0;
            bool Name            = true;
            bool Distance        = true;
            float DrawDistance   = 200.0f;
            float Color[4]       = { 0.55f, 0.85f, 1.00f, 1.00f };

            bool Recycler        = true;
            bool Cupboard        = true;
            bool TC_HealthBar    = true;
            bool TC_Upkeep       = false;
            bool TC_ShowID       = false;
            bool Stashes         = true;
            bool SleepingBag     = false;
            bool RFReceiver      = true;
            bool RFBroadcaster   = true;
            bool HBHFSensor      = true;
            bool SeismicSensor   = true;
            bool LargeBattery    = false;
            bool Workbench       = true;
        };

        struct ChamsSettings {
            bool  Enabled       = false;
            int   Style         = 0;
            float VisColor[4]   = { 0.20f, 1.00f, 0.30f, 0.85f };
            float OccColor[4]   = { 1.00f, 0.20f, 0.20f, 0.85f };
        };

        struct ChamsCategory {
            ChamsSettings Player;
            ChamsSettings Friendly;
            ChamsSettings NPC;
            ChamsSettings Vehicle;
            ChamsSettings Deployable;
            bool DisablePlayerCulling = false;
            int  SubTab               = 0;
        };

        PlayerESPSettings     Player;
        NPCESPSettings        NPC;
        VehicleESPSettings    Vehicle;
        DeployableESPSettings Deployable;
        ChamsCategory         Chams;
    } VisualCfg;

    struct AntiAimConfig {
        bool  Enable         = false;
        bool  OnlyMoving     = false;
        bool  OnlyStanding   = false;
        bool  DisableOnAim   = true;
        int   ToggleKey      = 0;

        int   YawMode        = 1;
        float YawBase        = 180.0f;
        float YawJitter      = 45.0f;
        float SpinSpeed      = 220.0f;
        int   InverterKey    = 'X';
        bool  InverterState  = false;

        int   PitchMode      = 1;
        float PitchValue     = 89.0f;
        float PitchJitter    = 30.0f;

        bool  EnableRoll     = false;
        int   RollMode       = 0;
        float RollValue      = 25.0f;
        float RollJitterSpd  = 6.0f;

        bool  FakeDesync     = false;
        float DesyncAmount   = 58.0f;

        bool  ShowIndicator  = true;
    } AntiAimCfg;

    struct MiscConfig {
        bool Watermark = true;
        bool  Flyhack     = false;
        float FlySpeed    = 15.0f;
        bool  Spider      = false;
        bool  Speedhack   = false;
        float SpeedScale  = 1.5f;
        bool  NoGravity   = false;
        bool  LongRender  = false;
        float RenderDist  = 10000.0f;
        bool  InstaAttack = false;
        bool  FullAuto    = false;
        bool  SilentShots = false;
        bool  MeleeReach  = false;
        float MeleeReachV = 5.0f;
        bool  SuperEoka  = false;
        bool  FastBow    = false;
        bool  NoFallDmg  = false;
        bool  JumpShoot  = false;
        bool  FakeAdmin   = false;
        bool  RpcDos      = false;
        int   RpcDosKey   = 0x7A;
        int   RpcDosRate  = 200;
        bool  JsonDos     = false;
        int   JsonDosKey  = 0x7B;
        int   JsonDosSize = 65536;
    } MiscCfg;

    struct WorldConfig {
        bool TimeChanger = false;
        float TimeOfDay = 12.0f;
        bool NoFog = false;
        bool SkyChanger = false;
        float SkyColor[4] = { 0.40f, 0.55f, 0.85f, 1.0f };
        bool SkyColorChanger = false;
        float SkyColorTint[4] = { 0.30f, 0.50f, 1.00f, 1.0f };
        bool AmbientChanger = false;
        float AmbientColor[4] = { 0.55f, 0.55f, 0.60f, 1.0f };
        bool FOVChanger = false;
        float CameraFOV = 90.0f;
        bool NightMode = false;
        float NightModeAmount = 0.45f;
        bool BulletTracer = false;
        bool TracerRainbow = false;
        float TracerColor[4] = { 1.00f, 0.85f, 0.15f, 0.90f };
        float TracerLife = 2.0f;
        float TracerThickness = 1.5f;
    } WorldCfg;

    struct ParticleConfig {
        int Mode = 1; 
        int Count = 70;
    } ParticleCfg;

    struct SettingsConfig {
        float MenuAlpha = 0.96f;
        bool SaveOnExit = true;
        bool ShowKeybinds = true;
    } SettingsCfg;

    static Menu& Get() {
        static Menu instance;
        return instance;
    }

private:
    Menu() = default;

    char m_cfgNameBuf[64] = "default";
    int  m_cfgSelected = -1;

    enum class Tab { Aim, AntiAim, Weapon, Visual, World, Misc, Settings, COUNT };
    Tab m_currentTab = Tab::Aim;
    
    float m_tabIndicatorX = 0.0f;
    float m_tabIndicatorTargetX = 0.0f;
    float m_contentAlpha = 1.0f;
    float m_openAnim = 0.0f;
    bool m_isOpen = true;
    float m_deltaTime = 0.016f;
    std::chrono::steady_clock::time_point m_lastFrame;
    
    struct ToggleAnim {
        float knobPos = 0.0f;
        float bgColor = 0.0f;
    };
    std::unordered_map<ImGuiID, ToggleAnim> m_toggleAnims;
    std::unordered_map<ImGuiID, float> m_hoverAnims;
    
    struct Particle {
        ImVec2 pos;
        ImVec2 vel;
        float size;
        float randomSeed;
    };
    std::vector<Particle> m_particles;
    void RenderBackgroundParticles();

    void RenderHeader(ImDrawList* dl, ImVec2 pos, ImVec2 size);
    void RenderTabBar(ImDrawList* dl, ImVec2 pos, ImVec2 size);
    void RenderContent();
    void RenderVisualsTab();
    void RenderAntiAimTab();

    bool CustomToggle(const char* label, bool* v);
    bool CustomToggle(const char* label, bool* v, const char* helpTooltip);
    bool CustomSliderFloat(const char* label, float* v, float v_min, float v_max, const char* fmt = "%.1f");
    bool CustomSliderInt(const char* label, int* v, int v_min, int v_max);
    bool CustomCombo(const char* label, int* current_item, const char* const items[], int items_count);
    void CustomColorEdit(const char* label, float col[4]);
    void SectionHeader(const char* label);
    
    static float Lerp(float a, float b, float t);
    ImVec4 GetAccentColor();
    ImU32 GetAccentColorU32(float alpha = 1.0f);

    static constexpr float TAB_WIDTH = 92.0f;
    static constexpr float HEADER_HEIGHT = 50.0f;
    static constexpr float TAB_BAR_HEIGHT = 38.0f;
};
