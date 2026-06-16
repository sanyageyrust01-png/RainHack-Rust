// rust dumper made by martin

#include <cstdint>



namespace offsets { 

    namespace il2cpp {

        // RVA of il2cpp_gchandle_get_target inside GameAssembly.dll.
        // Re-resolve every game patch via IL2CPP dumper output.
        inline constexpr std::uintptr_t get_handle = 0xe8c3590;

    } //  il2cpp



    namespace base_networkable {

        // RVA of `BaseNetworkable.%a4a680d5d77e77f98e437fdb67505c3e44f22b9a_TypeInfo`
        // (the obfuscated static container that holds clientEntities).
        // Static-class hash and decryption keys/offsets shift every patch.
        inline constexpr std::uintptr_t typeinfo = 0xe525798;



        inline constexpr std::uint32_t static_fields = 0xb8;

        // Offset of clientEntities wrapper inside the static_fields blob.
        inline constexpr std::uint32_t client_entities = 0x18;

        // Offset of entityList wrapper inside the decrypted clientEntities object.
        inline constexpr std::uint32_t entity_list = 0x10;

        // Offset of BufferList<BaseNetworkable> (Values) inside the decrypted
        // entityList ListDictionary. Was 0x20 in pre-May 2026 builds.
        inline constexpr std::uint32_t buffer = 0x10;

        // BufferList layout: T[] array @ +0x10, int count @ +0x18.
        inline constexpr std::uint32_t entListBase = 0x10;

        inline constexpr std::uint32_t entLS = 0x18;

    } // base_networkable



    namespace main_camera {

        inline constexpr std::uintptr_t typeinfo = 0xe5ad918;



        inline constexpr std::uint32_t static_fields = 0xb8;

        // Resolve chain (per dumper): TypeInfo + 0xB8 -> static_fields,
        // static_fields + 0x8 -> Lazy<MainCamera>, Lazy + 0x10 -> Camera*.
        inline constexpr std::uint32_t instance = 0x8;

        inline constexpr std::uint32_t buffer = 0x10;

        inline constexpr std::uint32_t viewMatrix = 0x30c;

        inline constexpr std::uint32_t position = 0x454;

    } // main_camera



                                       

    namespace BasePlayer {

        // May 2026 build offsets (TypeDefIndex 430 in dump.cs).
        // Re-extracted via IL2CPP dumper, see RainHack offsets-update workflow memory.

        inline constexpr std::uintptr_t username         = 0x330;  // string

        inline constexpr std::uintptr_t movement         = 0x300;  // BaseMovement* (NOT in dumper, kept from prev build)

        // ModelState* pool ptr. NOT explicitly listed by the dumper — verified
        // per-build via xrefs of `[BasePlayer + N]; [+0x60] & flag`. Keeping
        // 0x320 from prev build; runtime ResolveModelStateFlagsOffset() in
        // misc.cpp re-detects the flags offset every game launch as a safety net.
        inline constexpr std::uintptr_t modelState       = 0x320;

        inline constexpr std::uintptr_t playerInput      = 0x448;  // PlayerInput*

        inline constexpr std::uintptr_t team             = 0x4e8;  // currentTeam (ulong)

        inline constexpr std::uintptr_t clActiveItem     = 0x518;  // Lazy<HeldEntity>

        inline constexpr std::uintptr_t playerModel      = 0x548;  // PlayerModel*

        inline constexpr std::uintptr_t fallDamageEffect = 0x650;  // GameObjectRef (kept from prev build)

        inline constexpr std::uintptr_t playerFlags      = 0x660;

        inline constexpr std::uintptr_t playerInventory  = 0x690;  // PlayerInventory*



        // Viewmodel pointer slot — the dumper does not list a direct
        // ViewModel field on BasePlayer in this build either. Kept for
        // source compatibility; world.cpp falls back to clActiveItem.heldEntity
        // resolution when this slot is invalid.

        inline constexpr std::uintptr_t viewModel = 0x3f8;

    } // BasePlayer



    namespace ModelState {

        // ModelState class is obfuscated as %fd816cb26c6c8980e94410d0be5b8bb9d31f3143

        // (TypeDefIndex 8114). The dump.cs layout LIES about the field semantics —

        // the 'uint @ 0x14' field is NOT flags (it's some unrelated counter).

        // The REAL flag bitfield is at +0x40 (declared as `int %cb29268c` in the

        // dump). Verified via static disasm:

        //

        //   BaseMountable$$UpdatePlayerModel:  or [rcx+40h], 4   (sets OnGround)

        //   PlayerWalkMovement$$xxx:           or [rXX+40h], 40h (sets Flying)

        //   BasePlayer$$%9a5ec3b9...           test [rax+40h], 1 (tests Ducked)

        //   BowWeapon$$ProcessSpectatorViewmodel: [rdx+40h] >> 7 & 1 (Aiming)

        //

        // Bit values verified through the asm patterns:

        //   0x01 = Ducked, 0x04 = OnGround, 0x40 = Flying, 0x80 = Aiming.

        //

        // NOTE: writing to +0x14 (the previous incorrect offset) clobbers a

        // random counter and silently breaks JumpShoot / pose detection.

        inline constexpr std::uintptr_t flags = 0x40;



        // Bit masks for ModelState.flags (verified live in current build).

        //

        // Confirmed via static disasm in the current GameAssembly.dll:

        //   bit 0x01 = Ducked    (BasePlayer$$%9a5ec3b9... tests it for crouch eye-offset)

        //   bit 0x04 = OnGround  (BaseMountable$$UpdatePlayerModel sets it)

        //   bit 0x40 = Flying    (PlayerWalkMovement methods set it for wingsuit/parachute)

        //   bit 0x80 = Aiming    (BowWeapon$$ProcessSpectatorViewmodel reads bit 7 for ADS)

        // Other bit positions inferred from prior builds and Rust [Flags] enum order.

        enum FlagBit : std::uint32_t {

            Ducked       = 0x00001,  // crouching

            Jumped       = 0x00002,  // airborne from jump

            OnGround     = 0x00004,

            Sleeping     = 0x00008,  // sleeper / dead body

            Sprinting    = 0x00010,

            OnLadder     = 0x00020,

            Flying       = 0x00040,  // parachute / wingsuit

            Aiming       = 0x00080,  // ADS

            Prone        = 0x00100,

            Mounted      = 0x00200,  // in a seat / mountable

            Relaxed      = 0x00400,

            OnPhone      = 0x00800,

            Crawling     = 0x01000,

            Loading      = 0x02000,

            HeadLook     = 0x04000,

            HasParachute = 0x08000,

            Blocking     = 0x10000,

            Ragdolling   = 0x20000,

            Catching     = 0x40000,

        };

    } // ModelState



    namespace PlayerInput {

        inline constexpr std::uintptr_t bodyAngles = 0x44;

        inline constexpr std::uintptr_t inputState = 0x28;

    } // PlayerInput



    namespace InputState {

        inline constexpr std::uintptr_t current = 0x20;

        inline constexpr std::uintptr_t previous = 0x10;

    } // InputState



    namespace InputMessage {

        inline constexpr std::uintptr_t buttons = 0x14;

        inline constexpr std::uintptr_t position = 0x1C;

        inline constexpr std::uintptr_t aimAngles = 0x28;

    } // InputMessage



    namespace BasePlayer_Movement {

        // BaseMovement pointer on BasePlayer. Renamed but kept for source

        // compatibility — new name is BasePlayer::movement (= 0x300).

        inline constexpr std::uintptr_t movement = 0x300;

    } // BasePlayer_Movement



    namespace PlayerWalkMovement {

        inline constexpr std::uintptr_t gravityMult     = 0x48;

        inline constexpr std::uintptr_t movementMode    = 0x50;

        inline constexpr std::uintptr_t groundAngle     = 0x54;

        inline constexpr std::uint32_t  slidingFlagMask = 0x2023;

    } // PlayerWalkMovement



    namespace BaseEntity {

        inline constexpr std::uintptr_t model = 0xf0;

    } // BaseEntity



    namespace Item {

        // May 2026 build (Item discovery type:
        // %7a5da808f7c5df743aa6cb0bf2e54e28ec49567f per dumper output).
        //
        //   0x38  ulong          itemUid           (verified)
        //   0x40  HeldEntity     heldEntity        (verified)
        //   0x50  int            amount candidate  (alt #1)
        //   0x58  ItemContainer  parentContainer   (kept from prev build)
        //   0x80  HeldEntity     heldEntity2       (alt slot)
        //   0x94  int            amount candidate  (alt #2)
        //   0x98  ulong          uid alt slot
        //   0xC0  ItemDefinition info              (verified)
        //   0xC8  ulong          uid alt #3
        //   0xD0  ulong          uid alt #4

        inline constexpr std::uintptr_t itemDefinition = 0xc0;

        inline constexpr std::uintptr_t parentContainer = 0x58;

        inline constexpr std::uintptr_t itemUid    = 0x38;

        inline constexpr std::uintptr_t itemUid2   = 0x98;

        inline constexpr std::uintptr_t itemUid3   = 0xc8;

        inline constexpr std::uintptr_t itemUid4   = 0xd0;

        inline constexpr std::uintptr_t heldEntity  = 0x40;

        inline constexpr std::uintptr_t heldEntity2 = 0x80;

        // Legacy fallback slot from earlier builds — runtime helpers walk
        // the candidate list, so leave this even though dumper no longer
        // lists it as primary.
        inline constexpr std::uintptr_t heldEntityAlt = 0xb0;

        // amount: dumper offered candidates 0x50 and 0x94. Real position
        // is buildable via runtime IL2CPP class introspection.
        inline constexpr std::uintptr_t amount = 0x94;

        inline constexpr std::uintptr_t health = 0;

        inline constexpr std::uintptr_t maxHealth = 0;

    } // Item



    namespace BaseCombatEntity {

        inline constexpr std::uintptr_t lifeState = 0x278;

        inline constexpr std::uintptr_t health    = 0x284;

        inline constexpr std::uintptr_t maxHealth = 0x288;

    } // BaseCombatEntity



    namespace BaseProjectile {

        inline constexpr std::uintptr_t recoilProp      = 0x3b0;

        inline constexpr std::uintptr_t primaryMagazine = 0x388;

    } // BaseProjectile



    namespace BaseViewModel {

        inline constexpr std::uintptr_t BaseViewModel_C = 0xe5205e0;

        inline constexpr std::uintptr_t animationEvents = 0xc8;

        inline constexpr std::uintptr_t list            = 0x248;

    } // BaseViewModel



    namespace ItemContainer {

        inline constexpr std::uintptr_t list = 0x50;

    } // ItemContainer



    namespace ItemDefinition {

        inline constexpr std::uintptr_t shortName       = 0x28;

        inline constexpr std::uintptr_t itemDisplayName = 0x40;

        inline constexpr std::uintptr_t itemModWearable = 0x158;

        inline constexpr std::uintptr_t itemDisplayEnglish = 0;

    } // ItemDefinition



    namespace ListComponent_Projectile {

        inline constexpr std::uintptr_t ListComponent_C = 0xe4fe9b8;

        inline constexpr std::uintptr_t static_fields  = 0xb8;

        inline constexpr std::uintptr_t parent_static  = 0x10;

        inline constexpr std::uintptr_t buffer         = 0x10;

    } // ListComponent_Projectile



    namespace Magazine {

        inline constexpr std::uintptr_t Capacity = 0;

        inline constexpr std::uintptr_t Contents = 0;

    } // Magazine



    namespace Model {

        inline constexpr std::uintptr_t rootBone = 0x28;

        inline constexpr std::uintptr_t headBone = 0x30;

        inline constexpr std::uintptr_t boneTransforms = 0x50;

    } // Model



    namespace PlayerEyes {

        // May 2026 build re-shifted PlayerEyes back so that the raw Vector3
        // viewOffset lives at +0x40 again (was +0x60 in the Apr build that
        // wrapped it inside a Lazy<Vector3> slot).

        inline constexpr std::uintptr_t thirdPersonSleepingOffset = 0x28;

        inline constexpr std::uintptr_t viewOffset = 0x40;

        // Kept for source compatibility — earlier code paths that still
        // expect a wrapper at this slot will simply read the same Vector3.
        inline constexpr std::uintptr_t lazyAimWrapper = 0x40;

        inline constexpr std::uintptr_t bodyRotation = 0x50;

        inline constexpr std::uintptr_t eyeRotation = 0;

        inline constexpr std::uintptr_t unkQuanternion = 0x6c;

    } // PlayerEyes



    namespace PlayerInventory {

        inline constexpr std::uintptr_t container1 = 0x38;

        inline constexpr std::uintptr_t container2 = 0x58;

        inline constexpr std::uintptr_t container3 = 0x60;

    } // PlayerInvetory



    namespace PlayerModel {

        inline constexpr std::uintptr_t position         = 0x210;

        inline constexpr std::uintptr_t velocity         = 0x234;

        inline constexpr std::uintptr_t newVelocity      = 0;

        inline constexpr std::uintptr_t SkinnedMultiMesh = 0x350;

    } // PlayerModel



    namespace RecoilProperties {

        inline constexpr std::uintptr_t recoilYawMin = 0x18;

        inline constexpr std::uintptr_t recoilYawMax = 0x1c;

        inline constexpr std::uintptr_t recoilPitchMin = 0x20;

        inline constexpr std::uintptr_t recoilPitchMax = 0x24;

        inline constexpr std::uintptr_t newRecoilOverride = 0x80;

    } // RecoilProperties



    namespace SkinnedMultiMesh {

        inline constexpr std::uintptr_t rendererList = 0x40;

    } // SkinnedMultiMesh



    namespace TOD_Sky {

        // RVA of `TOD_Sky.%3d8ced3baa20f1d5fb248f8204f91a8d29ae0582_TypeInfo`
        // (May 2026 build). This is the typeinfo slot that TOD_Sky's OWN
        // methods reference at runtime (~469 xrefs from `TOD_Sky$$...`),
        // hence it's the one that gets initialized first by IL2CPP loader.
        //
        // DO NOT switch to the visually-named `TOD_Sky_TypeInfo` global
        // (0xe5b4548) — that slot is only referenced by `TOD_Camera$$...`
        // and stays uninitialized at the moment ESP::Render reads it,
        // which crashes `il2cpp_class_get_name` on the garbage pointer.
        //
        // The by-name lookup in world.cpp ResolveAll() handles the
        // "obfuscated class name" disagreement automatically.
        inline constexpr std::uintptr_t TOD_Sky_C = 0xe521468;

    } // namespace TOD_Sky

} // namespace offsets



