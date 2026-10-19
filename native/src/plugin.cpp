#include <ISmmPlugin.h>
#include <iserver.h>
#include <eiface.h>
#include <entity2/entitysystem.h>
#include <gametrace.h>
#include <schemasystem/schemasystem.h>
#include <interfaces/interfaces.h>
#include <json.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_set>
#include <vector>
#include <future>
#include <chrono>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cerrno>
#include <sys/resource.h>
#include "rules.h"
#include "corner_prediction.h"
#include "m_visibility.h"
#include "targeting.h"
#include "vis.h"
#include "transmit.h"
#include "memory.h"
#include "jsonc.h"
#include "config.h"
#include "smoke_memory.h"
#include <igameevents.h>

PLUGIN_GLOBALVARS();
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace awh;

namespace
{
    template <class T> T& Field( void* object, int offset )
    {
        return *reinterpret_cast<T*>( static_cast<uint8_t*>( object ) + offset );
    }

    CEntityInstance* EntityAt( CGameEntitySystem* system, CEntityIndex index )
    {
        const int value = index.Get();
        if ( !system || value < 0 || value >= MAX_TOTAL_ENTITIES - 1 )
            return nullptr;

        auto* chunk = system->m_EntityList.m_pIdentityChunks[value / MAX_ENTITIES_IN_LIST];
        if ( !chunk )
            return nullptr;

        auto& identity = chunk[value % MAX_ENTITIES_IN_LIST];

        return identity.GetEntityIndex() == index ? identity.m_pInstance : nullptr;
    }

    CEntityInstance* EntityAt( CGameEntitySystem* system, const CEntityHandle& handle )
    {
        if ( !handle.IsValid() )
            return nullptr;

        auto* entity = EntityAt( system, CEntityIndex( handle.GetEntryIndex() ) );

        return entity && entity->GetRefEHandle() == handle ? entity : nullptr;
    }

    bool Finite( const Vector& v )
    {
        return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
    }

    bool IsClass( CEntityInstance* entity, const char* name )
    {
        if ( !entity || !entity->m_pEntity )
            return false;

        auto* info = entity->Schema_DynamicBinding().m_pObj;

        for ( int depth = 0; info && depth < 32; ++depth )
        {
            if ( !std::strcmp( info->m_pszName, name ) )
                return true;

            info = info->m_nBaseClassCount ? info->m_pBaseClasses[0].m_pClass : nullptr;
        }

        return false;
    }

    struct Offsets
    {
        int pawn, currentPawn, pawnController, spawning, connected, flags, hltv, team, health, life, body, scene, collision, velocity, view, angles;
        int origin, child, sibling, nodeOwner, mins, maxs, owner, weapons, weaponHandles;
        int rules, warmup, freeze, winner;
        int movement = -1, buttons = -1, buttonStates = -1;

        static int Find( CSchemaSystemTypeScope* scope, const char* cls, const char* name )
        {
            auto* info = scope->FindDeclaredClass( cls ).Get();

            for ( int depth = 0; info && depth < 32; ++depth )
            {
                if ( !Readable( info, sizeof( *info ) ) || info->m_nFieldCount > 4096 || info->m_nBaseClassCount > 32 ||
                     ( info->m_nFieldCount && !Readable( info->m_pFields, size_t( info->m_nFieldCount ) * sizeof( SchemaClassFieldData_t ) ) ) )
                {
                    META_CONPRINTF( "[AntiWallHack] Invalid schema metadata: %s\n", cls );
                    return -1;
                }

                for ( int i = 0; i < info->m_nFieldCount; ++i )
                    if ( SchemaNameEquals( info->m_pFields[i].m_pszName, name ) )
                    {
                        int offset = info->m_pFields[i].m_nSingleInheritanceOffset;
                        if ( offset < 0 || offset >= info->m_nSize )
                        {
                            META_CONPRINTF( "[AntiWallHack] Invalid schema offset: %s::%s\n", cls, name );
                            return -1;
                        }

                        return offset;
                    }
                if ( info->m_nBaseClassCount && !Readable( info->m_pBaseClasses, size_t( info->m_nBaseClassCount ) * sizeof( SchemaBaseClassInfoData_t ) ) )
                {
                    META_CONPRINTF( "[AntiWallHack] Invalid schema base metadata: %s\n", cls );
                    return -1;
                }

                info = info->m_nBaseClassCount ? info->m_pBaseClasses[0].m_pClass : nullptr;
            }

            META_CONPRINTF( "[AntiWallHack] Missing schema field: %s::%s\n", cls, name );

            return -1;
        }

        bool Init( CSchemaSystemTypeScope* s )
        {
            pawn = Find( s, "CCSPlayerController", "m_hPlayerPawn" );
            currentPawn = Find( s, "CBasePlayerController", "m_hPawn" );
            pawnController = Find( s, "CBasePlayerPawn", "m_hController" );
            spawning = Find( s, "CCSPlayerPawn", "m_bIsSpawning" );
            connected = Find( s, "CBasePlayerController", "m_iConnected" );
            flags = Find( s, "CBaseEntity", "m_fFlags" );
            hltv = Find( s, "CBasePlayerController", "m_bIsHLTV" );
            team = Find( s, "CBaseEntity", "m_iTeamNum" );
            health = Find( s, "CBaseEntity", "m_iHealth" );
            life = Find( s, "CBaseEntity", "m_lifeState" );
            body = Find( s, "CBaseEntity", "m_CBodyComponent" );
            scene = Find( s, "CBodyComponent", "m_pSceneNode" );
            collision = Find( s, "CBaseEntity", "m_pCollision" );
            velocity = Find( s, "CBaseEntity", "m_vecAbsVelocity" );
            view = Find( s, "CBaseModelEntity", "m_vecViewOffset" );
            angles = Find( s, "CCSPlayerPawn", "m_angEyeAngles" );
            origin = Find( s, "CGameSceneNode", "m_vecAbsOrigin" );
            child = Find( s, "CGameSceneNode", "m_pChild" );
            sibling = Find( s, "CGameSceneNode", "m_pNextSibling" );
            nodeOwner = Find( s, "CGameSceneNode", "m_pOwner" );
            mins = Find( s, "CCollisionProperty", "m_vecMins" );
            maxs = Find( s, "CCollisionProperty", "m_vecMaxs" );
            owner = Find( s, "CBaseEntity", "m_hOwnerEntity" );
            weapons = Find( s, "CBasePlayerPawn", "m_pWeaponServices" );
            weaponHandles = Find( s, "CPlayer_WeaponServices", "m_hMyWeapons" );
            rules = Find( s, "CCSGameRulesProxy", "m_pGameRules" );
            warmup = Find( s, "CCSGameRules", "m_bWarmupPeriod" );
            freeze = Find( s, "CCSGameRules", "m_bFreezePeriod" );
            winner = Find( s, "CCSGameRules", "m_iRoundWinStatus" );

            for ( int value : { pawn, currentPawn, pawnController, spawning, connected,     flags,  hltv,   team,   health,  life,
                                body, scene,       collision,      velocity, view,          angles, origin, child,  sibling, nodeOwner,
                                mins, maxs,        owner,          weapons,  weaponHandles, rules,  warmup, freeze, winner } )
                if ( value < 0 )
                    return false;

            return true;
        }
    };

    struct Snapshot
    {
        CEntityInstance* pawn = nullptr;
        uint32 controllerHandle = 0, pawnHandle = 0;
        uint8 team = 0;
        bool human = false, complete = false;
        Vector origin, eye, mins, maxs, velocity;
        QAngle angles;
        std::vector<CEntityHandle> entities;
        std::array<Vector, 12> samples, smokeSamples;
        bool samplesReady = false;
        int visCell = -1;
        uint64_t movementButtons = 0;
        Vector eyeLead{ 0, 0, 0 };
        bool eyeLeadReady = false;
        int visPeekCell = -1;
        int health = 0, life = -1;
    };

    struct Identity
    {
        uint32 controller = 0, pawn = 0;
        uint8 team = 0;

        bool operator==( const Identity& b ) const
        {
            return controller == b.controller && pawn == b.pawn && team == b.team;
        }
    };

    class SightFilter final : public CTraceFilter
    {
        const Snapshot& viewer;
        const Snapshot& target;

      public:
        SightFilter( const Snapshot& a, const Snapshot& b ) : CTraceFilter( uint64( 1 ) << LAYER_INDEX_CONTENTS_SOLID ), viewer( a ), target( b )
        {
            m_nInteractsExclude = ( uint64( 1 ) << LAYER_INDEX_CONTENTS_PLAYER ) | ( uint64( 1 ) << LAYER_INDEX_CONTENTS_HITBOX ) |
                                  ( uint64( 1 ) << LAYER_INDEX_CONTENTS_TRIGGER ) | ( uint64( 1 ) << LAYER_INDEX_CONTENTS_PLAYER_CLIP ) |
                                  ( uint64( 1 ) << LAYER_INDEX_CONTENTS_NPC_CLIP ) | ( uint64( 1 ) << LAYER_INDEX_CONTENTS_WINDOW ) |
                                  ( uint64( 1 ) << LAYER_INDEX_CONTENTS_CARRIED_OBJECT ) | ( uint64( 1 ) << LAYER_INDEX_CONTENTS_CARRIED_WEAPON );
            SetPassEntity1( a.pawn );
            SetPassEntity2( b.pawn );
            SetPassEntityOwner1( a.pawn );
            SetPassEntityOwner2( b.pawn );
        }

        bool ShouldHitEntity( CEntityInstance* e ) override
        {
            if ( !e )
                return true; // world geometry

            if ( !e->m_pEntity || IsClass( e, "CBasePlayerPawn" ) || IsClass( e, "CBasePlayerController" ) || IsClass( e, "CEconEntity" ) )
                return false;

            auto handle = e->GetRefEHandle();

            return std::find( viewer.entities.begin(), viewer.entities.end(), handle ) == viewer.entities.end() &&
                   std::find( target.entities.begin(), target.entities.end(), handle ) == target.entities.end();
        }
    };
}

class AntiWallHack final : public ISmmPlugin, public IMetamodListener
{
    using TraceFunction = void ( * )( void*, Ray_t&, Vector&, Vector&, CTraceFilter*, CGameTrace* );
    using TraceHook = KHook::Function<void, void*, Ray_t&, Vector&, Vector&, CTraceFilter*, CGameTrace*>;
    ISource2GameEntities* gameEntities = nullptr;
    ISource2Server* serverDll = nullptr;
    using FrameHook = KHook::Virtual<ISource2Server, void, bool, bool, bool>;
    std::unique_ptr<FrameHook> frameHook;
    INetworkServerService* network = nullptr;
    void* resources = nullptr;
    CGameEntitySystem* entitySystem = nullptr;
    Offsets offsets;
    Settings settings;
    fs::path configPath, gamePath;
    VisibilityMap visibility;
    std::string visMapName;
    std::string visLoadingMap;
    uint64_t visGeneration = 0, visLoadingGeneration = 0;
    fs::file_time_type visObservedTime{};
    uintmax_t visObservedSize = 0;
    bool visObserved = false, visObservedExists = false;
    std::mutex debugMutex;
    unsigned long long debugSequence = 0;
    std::atomic<bool> debugTraceSeen{ false };
    bool debugTransmitSeen = false, debugTransmitReady = false, debugFrameSeen = false;
    std::shared_ptr<std::atomic<bool>> visCancel;
    // Destroy/join the future before the logger members used by its worker.
    std::future<VisibilityMap> visPending;

    void DebugStage( const char* stage ) noexcept
    {
        if ( !settings.Debug )
            return;
        try
        {
            std::lock_guard<std::mutex> lock( debugMutex );
            auto sequence = ++debugSequence;
            std::fprintf( stderr, "[AntiWallHack][Debug] 1.0.3 #%llu %s\n", sequence, stage );
            std::fflush( stderr );
            if ( gamePath.empty() )
                return;
            auto path = gamePath / "addons/antiwallhack/startup-debug.log";
            if ( auto* log = std::fopen( path.string().c_str(), "a" ) )
            {
                std::fprintf( log, "[AntiWallHack][Debug] 1.0.3 #%llu %s\n", sequence, stage );
                std::fflush( log );
                std::fclose( log );
            }
        }
        catch ( ... )
        {
        } // Diagnostics must never interrupt engine callbacks.
    }

    std::chrono::steady_clock::time_point visNextPoll{};
    std::chrono::steady_clock::time_point visNextNotice{};
    std::string visStatus = "waiting for map physics before VIS validation";

    void NotifyMissingVisibility()
    {
        if ( !settings.Enabled || visMapName.empty() )
            return;
        if ( visibility.Ready() )
        {
            visNextNotice = {};
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if ( now < visNextNotice )
            return;
        visNextNotice = now + std::chrono::minutes( 5 );
        META_CONPRINTF( "[AntiWallHack] Checks disabled for %s: %s. Will resume automatically when VIS is ready.\n", visMapName.c_str(), visStatus.c_str() );
    }

    KHook::Return<void> OnFrame( ISource2Server*, bool, bool, bool )
    {
        // File-only validation needs map identity/physics readiness, not a human
        // client's CheckTransmit callback. Keep empty servers prepared as well.
        if ( settings.Enabled && traceManager && !visMapName.empty() )
            UpdateVisibility();
        NotifyMissingVisibility();
        return { KHook::Action::Ignore };
    }

    class EventHook final : public KHook::Virtual<IGameEventManager2, bool, IGameEvent*, bool>
    {
        using Base = KHook::Virtual<IGameEventManager2, bool, IGameEvent*, bool>;

      public:
        using Base::Base;

        bool Installed( void** vtable, int index )
        {
            std::lock_guard guard( this->_hooks_stored );
            return this->_addr_hook_ids.count( vtable + index ) != 0;
        }
    };

    std::unique_ptr<EventHook> eventHook;
    IGameEventManager2* eventManager = nullptr;
    uintptr_t eventManagerAddress = 0;
    bool eventHooked = false, smokeAvailable = false, smokeCaptured = false;
    int smokeEffectOffset = -1, smokeCellBudget = 262144;
    std::chrono::steady_clock::time_point smokeNextWarning{};
    float frameTime = 0;
    std::array<std::unique_ptr<smoke::Volume>, 32> smokeStorage;
    std::vector<const smoke::Volume*> smokeVolumes;
    std::vector<uint8_t> smokeFrame = std::vector<uint8_t>( smoke::FrameBytes );
    smoke::Explosions smokeExplosions;
    int entitySystemOffset = 0;
    std::unique_ptr<TraceHook> traceHook;
    TraceFunction traceFunction = nullptr;
    void* traceManager = nullptr;
    using TransmitHook = KHook::Virtual<ISource2GameEntities, void, CCheckTransmitInfo**, int, CBitVec<16384>&, CBitVec<16384>&, const Entity2Networkable_t**,
                                        const uint16*, int>;
    std::unique_ptr<TransmitHook> transmitHook;
    std::array<Snapshot, 64> players;
    std::array<Identity, 64> identities{};
    int deadlines[64][64]{};
    int preferred[64][64]{};
    std::array<std::vector<CEntityHandle>, 64> hidden;
    std::array<bool, 64> evaluated{};
    PairVisibilityCache pairVisibility;
    int frameTick = -1, traces = 0;
    bool faulted = false, hooked = false, convarChanged = false;
    std::string previousConvar;

    Json ReadGamedata( const fs::path& path )
    {
        std::ifstream file( path );
        if ( !file )
            throw std::runtime_error( "Cannot open " + path.string() );

        return ParseJsonc( std::string( std::istreambuf_iterator<char>( file ), {} ) );
    }

    void LoadSettings()
    {
        if ( !fs::exists( configPath ) )
        {
            fs::create_directories( configPath.parent_path() );
            std::ofstream out( configPath );
            out << DefaultConfig;
            out.close();

            if ( !out )
                throw std::runtime_error( "Cannot create " + configPath.string() );
        }

        std::ifstream file( configPath );

        if ( !file )
            throw std::runtime_error( "Cannot open " + configPath.string() );

        settings = ParseConfig( file );
    }

    void LoadVisibility( const char* name, bool startValidation = true )
    {
        if ( visCancel )
            visCancel->store( true );
        DebugStage( "LoadVisibility: enter" );
        visibility = {};
        visMapName = name && *name ? name : "<unknown>";
        ++visGeneration;
        visObserved = false;
        visNextPoll = {};
        visNextNotice = {};
        visStatus = "waiting for map physics before VIS validation";

        if ( startValidation )
            UpdateVisibility();
        else
            DebugStage( "VIS: validation deferred until ready frame or transmit" );
        DebugStage( "LoadVisibility: returned" );
    }

    void UpdateVisibility()
    {
        using namespace std::chrono;
        if ( visPending.valid() )
        {
            if ( visPending.wait_for( seconds( 0 ) ) != std::future_status::ready )
                return;
            try
            {
                auto loaded = visPending.get();
                if ( visLoadingMap == visMapName && visLoadingGeneration == visGeneration )
                {
                    visibility = std::move( loaded );
                    frameTick = -1;
                    META_CONPRINTF( "[AntiWallHack] VIS activated for %s: %zu cells, all enemies eligible.\n", visMapName.c_str(), visibility.CellCount() );
                }
            }
            catch ( const std::exception& error )
            {
                if ( visLoadingMap == visMapName && visLoadingGeneration == visGeneration )
                {
                    visibility = {};
                    visStatus = "VIS validation failed: " + std::string( error.what() );
                    DebugStage( error.what() );
                    // Workshop downloads may finish after the first validation attempt.
                    visObserved = false;
                    visNextPoll = steady_clock::now() + seconds( 30 );
                }
            }
        }
        auto now = steady_clock::now();
        if ( now < visNextPoll )
            return;
        visNextPoll = now + seconds( 5 );
        if ( visMapName.empty() || visMapName != MapIdentity( visMapName ) )
        {
            return;
        }
        try
        {
            auto path = gamePath / "addons/antiwallhack/vis" / ( visMapName + ".awvis" );
            bool exists = fs::is_regular_file( path );
            auto time = exists ? fs::last_write_time( path ) : fs::file_time_type{};
            auto size = exists ? fs::file_size( path ) : 0;
            if ( visObserved && exists == visObservedExists && time == visObservedTime && size == visObservedSize )
                return;
            if ( !exists )
            {
                visibility = {};
                visStatus = "baked VIS table missing: " + path.string();
                visObserved = true;
                visObservedExists = false;
                visObservedTime = time;
                visObservedSize = size;
                return;
            }
            auto root = gamePath;
            auto map = visMapName;
            visStatus = "validating baked VIS table: " + path.string();
            visLoadingMap = map;
            visLoadingGeneration = visGeneration;
            visCancel = std::make_shared<std::atomic<bool>>( false );
            auto cancelled = visCancel;
            DebugStage( "VIS: launching validation worker" );
            visPending = std::async( std::launch::async,
                                     [this, path, root, map, cancelled]
                                     {
                                         const auto tid = static_cast<id_t>( syscall( SYS_gettid ) );
                                         errno = 0;
                                         const int currentNice = getpriority( PRIO_PROCESS, tid );
                                         if ( errno == 0 && ( currentNice >= 15 || setpriority( PRIO_PROCESS, tid, 15 ) == 0 ) )
                                             DebugStage( "VIS worker: low priority enabled" );
                                         else
                                             DebugStage( "VIS worker: priority unchanged; throttled I/O still enabled" );
                                         DebugStage( "VIS worker: entered" );
                                         VisibilityMap loaded;
                                         try
                                         {
                                             loaded.Load(
                                                 path, root, map,
                                                 [this]( const char* progress )
                                                 {
                                                     DebugStage( progress );
                                                 },
                                                 [cancelled]
                                                 {
                                                     return cancelled->load();
                                                 } );
                                         }
                                         catch ( ... )
                                         {
                                             DebugStage( "VIS worker: validation threw" );
                                             throw;
                                         }
                                         DebugStage( "VIS worker: validation completed" );
                                         return loaded;
                                     } );
            DebugStage( "VIS: validation worker launched" );
            visObserved = true;
            visObservedExists = exists;
            visObservedTime = time;
            visObservedSize = size;
        }
        catch ( const std::exception& error )
        {
            visibility = {};
            visStatus = "VIS unavailable: " + std::string( error.what() );
            DebugStage( error.what() );
        }
    }

    void Reset()
    {
        frameTick = -1;
        traces = 0;
        std::memset( deadlines, 0, sizeof( deadlines ) );
        std::memset( preferred, 0, sizeof( preferred ) );
        identities.fill( {} );
        evaluated.fill( false );
        pairVisibility.Reset();

        for ( auto& p : players )
            p = Snapshot{};

        for ( auto& row : hidden )
            row.clear();
    }

    void ConfigureConvar()
    {
        ConVarRefAbstract cvar( "sv_enable_donttransmit" );

        if ( !cvar.IsValidRef() )
            return;

        if ( !convarChanged )
        {
            previousConvar = cvar.GetString().Get();
            convarChanged = true;
        }

        cvar.SetBool( false );
    }

    void DisableSmoke( const std::string& reason )
    {
        smokeAvailable = false;
        smokeVolumes.clear();
        evaluated.fill( false );
        pairVisibility.Reset();
        for ( auto& row : hidden )
            row.clear();
        META_CONPRINTF( "[AntiWallHack] Smoke filtering disabled until plugin reload: %s. Wall filtering continues.\n", reason.c_str() );
    }

    void RemoveEventHook()
    {
        if ( eventHooked && eventHook && eventManager )
            eventHook->Remove( eventManager );

        eventHooked = false;
        eventHook.reset();
        eventManager = nullptr;
    }

    void SetupSmoke( void* serverFunction, CSchemaSystemTypeScope* scope, const Json& platform )
    {
        DebugStage( "Smoke: setup entered" );
        if ( !settings.SmokeOcclusion )
        {
            META_CONPRINTF( "[AntiWallHack] Smoke filtering disabled by SmokeOcclusion 0.\n" );
            return;
        }

        std::string reason;
        DebugStage( "Smoke: verifying server fingerprint" );

        if ( !smoke::VerifyModule( serverFunction, reason ) )
        {
            DisableSmoke( reason );
            return;
        }
        try
        {
            DebugStage( "Smoke: fingerprint accepted; resolving schema/event manager" );
            smokeEffectOffset = Offsets::Find( scope, "CSmokeGrenadeProjectile", "m_bDidSmokeEffect" );

            if ( smokeEffectOffset < 0 )
                throw std::runtime_error( "smoke schema field unavailable" );

            auto* signature = static_cast<uint8_t*>( FindServerFunction( platform.at( "GameEventManager" ).get<std::string>(), serverFunction ) );
            int32_t displacement = 0;

            if ( !signature || !smoke::ReadValue( reinterpret_cast<uintptr_t>( signature ) + 3, displacement ) )
                throw std::runtime_error( "HE event manager signature unavailable" );

            auto address = reinterpret_cast<uintptr_t>( signature ) + 7 + displacement;
            eventManagerAddress = address;

            if ( !smoke::ReadValue( address, eventManager ) || !eventManager )
                throw std::runtime_error( "HE event manager unavailable" );

            void** vtable = nullptr;
            int index = platform.at( "FireEvent" ).get<int>();
            void* function = nullptr;

            if ( index != KHook::GetVtableIndex( &IGameEventManager2::FireEvent ) || !smoke::ReadValue( reinterpret_cast<uintptr_t>( eventManager ), vtable ) ||
                 !vtable || !smoke::ReadValue( reinterpret_cast<uintptr_t>( vtable + index ), function ) || !function )
                throw std::runtime_error( "HE event dispatch layout mismatch" );

            eventHook = std::make_unique<EventHook>( &IGameEventManager2::FireEvent, this, &AntiWallHack::OnGameEvent, nullptr );
            DebugStage( "Smoke: installing HE event hook" );
            eventHook->Add( eventManager );
            eventHooked = true;

            if ( !eventHook->Installed( vtable, index ) )
                throw std::runtime_error( "HE event hook installation failed" );

            smokeAvailable = true;
            DebugStage( "Smoke: HE hook installed and smoke ready" );
            META_CONPRINTF( "[AntiWallHack] Smoke filtering enabled with verified voxel layout.\n" );
        }
        catch ( const std::exception& ex )
        {
            RemoveEventHook();
            DisableSmoke( ex.what() );
        }
    }

    void CaptureSmoke()
    {
        smokeCaptured = true;
        smokeVolumes.clear();

        if ( !smokeAvailable || !smokeExplosions.Begin( frameTime ) )
            return;

        IGameEventManager2* currentManager = nullptr;

        if ( !smoke::ReadValue( eventManagerAddress, currentManager ) || currentManager != eventManager )
        {
            DisableSmoke( "HE event manager changed or became unreadable" );
            return;
        }
        try
        {
            size_t count = 0;

            for ( auto* identity = entitySystem->m_EntityList.m_pFirstActiveEntity; identity; identity = identity->m_pNext )
            {
                auto* entity = identity->m_pInstance;

                if ( !entity || !IsClass( entity, "CSmokeGrenadeProjectile" ) )
                    continue;

                uint8_t active = 0;

                if ( !smoke::ReadValue( reinterpret_cast<uintptr_t>( entity ) + smokeEffectOffset, active ) )
                {
                    DisableSmoke( "unreadable smoke effect state" );
                    return;
                }

                if ( active > 1 )
                {
                    DisableSmoke( "invalid smoke effect state" );
                    return;
                }

                if ( !active )
                    continue;

                if ( count == smokeStorage.size() )
                {
                    smokeVolumes.clear();
                    return;
                }

                auto& volume = smokeStorage[count];

                if ( !volume )
                    volume = std::make_unique<smoke::Volume>();
                constexpr size_t volumeOffset = 3736;
                if ( !smoke::ReadSnapshot( reinterpret_cast<uintptr_t>( entity ), volumeOffset, frameTime, *volume, smokeFrame, smoke::ReadMemory ) )
                {
                    // An unpublished/unstable voxel frame only invalidates this tick.
                    smokeVolumes.clear();
                    auto now = std::chrono::steady_clock::now();

                    if ( now >= smokeNextWarning )
                    {
                        smokeNextWarning = now + std::chrono::seconds( 30 );
                        META_CONPRINTF( "[AntiWallHack] Smoke snapshot unavailable; revealing this tick and retrying. Wall filtering continues.\n" );
                    }

                    return;
                }

                ++count;

                if ( !smokeExplosions.Clears( *volume, frameTime ) )
                    smokeVolumes.push_back( volume.get() );
            }
        }
        catch ( const std::exception& ex )
        {
            DisableSmoke( ex.what() );
        }
    }

    bool IsSmokeHidden( const Snapshot& viewer, const Snapshot& target, Vector eyeLead, int count, int first )
    {
        if ( !smokeAvailable || !settings.SmokeOcclusion )
            return false;

        if ( !smokeCaptured )
            CaptureSmoke();

        if ( !smokeAvailable || smokeVolumes.empty() )
            return false;

        return smoke::SamplesBlocked( smokeVolumes, count, first, settings.CornerPrediction ? settings.CornerPredictionSeconds : 0, smokeCellBudget,
                                      [&]( int index )
                                      {
                                          bool predicted = index >= 12;

                                          Vector start = viewer.eye + ( predicted ? eyeLead : Vector( 0, 0, 0 ) );
                                          Vector end = target.smokeSamples[index % 12];

                                          return std::make_pair( smoke::Point{ start.x, start.y, start.z }, smoke::Point{ end.x, end.y, end.z } );
                                      } );
    }

    bool CollectChildren( void* node, Snapshot& snapshot, std::unordered_set<void*>& seen, int depth )
    {
        while ( node )
        {
            if ( depth > 16 || seen.size() >= 128 || !seen.insert( node ).second )
            {
                return false;
            }

            auto* entity = Field<CEntityInstance*>( node, offsets.nodeOwner );

            if ( !entity || !entity->m_pEntity )
            {
                return false;
            }
            if ( IsClass( entity, "CBasePlayerPawn" ) || IsClass( entity, "CBasePlayerController" ) )
            {
                return false;
            }

            snapshot.entities.push_back( entity->GetRefEHandle() );

            if ( !CollectChildren( Field<void*>( node, offsets.child ), snapshot, seen, depth + 1 ) )
                return false;

            node = Field<void*>( node, offsets.sibling );
        }

        return true;
    }

    bool RoundActive()
    {
        for ( auto* id = entitySystem->m_EntityList.m_pFirstActiveEntity; id; id = id->m_pNext )
        {
            auto* e = id->m_pInstance;

            if ( !e || !IsClass( e, "CCSGameRulesProxy" ) )
                continue;

            auto* rules = Field<void*>( e, offsets.rules );

            return rules && !Field<bool>( rules, offsets.warmup ) && !Field<bool>( rules, offsets.freeze ) && Field<int>( rules, offsets.winner ) == 0;
        }

        return false;
    }

    void BeginFrame( int tick, float now )
    {
        if ( frameTick == tick )
            return;

        if ( tick < frameTick )
        {
            Reset();
            smokeExplosions.Reset();
        }

        frameTick = tick;
        frameTime = now;
        traces = 0;
        smokeCaptured = false;
        smokeVolumes.clear();
        smokeCellBudget = 262144;

        evaluated.fill( false );
        pairVisibility.Reset();

        for ( auto& row : hidden )
            row.clear();

        for ( int slot = 0; slot < 64; ++slot )
        {
            auto& p = players[slot];
            p = Snapshot{};
            auto* controller = EntityAt( entitySystem, CEntityIndex( slot + 1 ) );

            if ( !controller || !IsClass( controller, "CCSPlayerController" ) )
            {
                identities[slot] = {};
                continue;
            }
            p.team = Field<uint8>( controller, offsets.team );
            p.controllerHandle = controller->GetRefEHandle().ToInt();
            if ( Field<int>( controller, offsets.connected ) != 0 )
            {
                identities[slot] = {};
                continue;
            }

            auto handle = Field<CEntityHandle>( controller, offsets.pawn );
            auto* pawn = EntityAt( entitySystem, handle );

            if ( !pawn || !IsClass( pawn, "CCSPlayerPawn" ) )
            {
                identities[slot] = {};
                continue;
            }
            p.pawnHandle = handle.ToInt();
            p.health = Field<int>( pawn, offsets.health );
            p.life = Field<uint8>( pawn, offsets.life );
            bool spawning = Field<bool>( pawn, offsets.spawning );
            bool belongs = Field<CEntityHandle>( pawn, offsets.pawnController ) == controller->GetRefEHandle();
            if ( !LivePawn( p.health, uint8_t( p.life ), spawning, belongs ) )
            {
                identities[slot] = {};
                continue;
            }

            auto team = Field<uint8>( pawn, offsets.team );
            auto* body = Field<void*>( pawn, offsets.body );
            auto* scene = body ? Field<void*>( body, offsets.scene ) : nullptr;
            auto* collision = Field<void*>( pawn, offsets.collision );

            if ( !scene || !collision || ( team != 2 && team != 3 ) )
            {
                identities[slot] = {};
                continue;
            }

            p.origin = Field<Vector>( scene, offsets.origin );
            p.eye = p.origin + Field<Vector>( pawn, offsets.view );
            p.velocity = Field<Vector>( pawn, offsets.velocity );
            p.mins = Field<Vector>( collision, offsets.mins );
            p.maxs = Field<Vector>( collision, offsets.maxs );
            p.angles = Field<QAngle>( pawn, offsets.angles );

            if ( !Finite( p.origin ) || !Finite( p.eye ) || !Finite( p.velocity ) || !Finite( p.mins ) || !Finite( p.maxs ) || !std::isfinite( p.angles.x ) ||
                 !std::isfinite( p.angles.y ) || !std::isfinite( p.angles.z ) || p.mins.x >= p.maxs.x || p.mins.y >= p.maxs.y || p.mins.z >= p.maxs.z )
            {
                identities[slot] = {};
                continue;
            }

            p.pawn = pawn;
            p.team = team;
            p.pawnHandle = handle.ToInt();
            p.controllerHandle = controller->GetRefEHandle().ToInt();
            if ( settings.CornerPrediction && offsets.movement >= 0 && offsets.buttons >= 0 && offsets.buttonStates >= 0 )
                if ( auto* movement = Field<void*>( pawn, offsets.movement ) )
                    p.movementButtons = Field<uint64_t>( movement, offsets.buttons + offsets.buttonStates );
            // Locate once per player/tick, not once per enemy pair. Nonstandard hulls use rays.
            if ( visibility.Ready() && settings.BoundsPadding <= 38 && p.maxs.z <= 72 && p.mins.z >= 0 && p.mins.x >= -16 && p.mins.y >= -16 &&
                 p.maxs.x <= 16 && p.maxs.y <= 16 && p.eye.z - p.origin.z >= 40 && p.eye.z - p.origin.z <= 68 )
            {
                p.visCell = visibility.Cell( p.origin );
            }

            // FL_FAKECLIENT and HLTV bypass, only filter the actual living pawn, not a possessed bot.
            p.human = !( Field<uint32>( controller, offsets.flags ) & FL_FAKECLIENT ) && !Field<bool>( controller, offsets.hltv ) &&
                      Field<CEntityHandle>( controller, offsets.currentPawn ) == handle;
            Identity identity{ p.controllerHandle, p.pawnHandle, team };

            if ( !( identities[slot] == identity ) )
            {
                identities[slot] = identity;

                for ( int other = 0; other < 64; ++other )
                {
                    deadlines[slot][other] = deadlines[other][slot] = tick + settings.VisibleGraceTicks;
                    preferred[slot][other] = preferred[other][slot] = 0;
                }
            }

            p.entities.push_back( handle );
            std::unordered_set<void*> seen;
            p.complete = CollectChildren( Field<void*>( scene, offsets.child ), p, seen, 0 );
            auto* services = Field<void*>( pawn, offsets.weapons );

            if ( !services )
            {
                p.complete = false;
            }
            else
            {
                const auto& weapons = Field<CUtlVector<CEntityHandle>>( services, offsets.weaponHandles );

                if ( weapons.Count() < 0 || weapons.Count() > 64 )
                {
                    p.complete = false;
                }
                else
                    for ( int i = 0; i < weapons.Count(); ++i )
                    {
                        if ( EntityAt( entitySystem, weapons[i] ) )
                            p.entities.push_back( weapons[i] );
                        else if ( weapons[i].IsValid() )
                        {
                            p.complete = false;
                        }
                    }
            }
        }
        // Ownership does not imply equipment: never include thrown grenades/projectiles.
        for ( auto* id = entitySystem->m_EntityList.m_pFirstActiveEntity; id; id = id->m_pNext )
        {
            auto* e = id->m_pInstance;

            if ( !e || !IsClass( e, "CEconEntity" ) )
                continue;

            auto owner = Field<CEntityHandle>( e, offsets.owner );

            if ( !owner.IsValid() )
                continue;

            for ( auto& p : players )
                if ( p.pawn && owner.ToInt() == p.pawnHandle )
                {
                    p.entities.push_back( e->GetRefEHandle() );
                    break;
                }
        }
    }

    void FillSamples( const Snapshot& target, float padding, std::array<Vector, 12>& points )
    {
        auto lo = target.mins - Vector( padding, padding, 0 );
        auto hi = target.maxs + Vector( padding, padding, padding );
        points[0] = target.eye;
        points[1] = target.origin + ( lo + hi ) * .5f;
        for ( int i = 0; i < 8; ++i )
            points[i + 2] = target.origin + Vector( ( i & 1 ) ? hi.x : lo.x, ( i & 2 ) ? hi.y : lo.y, ( i & 4 ) ? hi.z : lo.z + 4 );
        constexpr float radians = 3.14159265358979323846f / 180;
        float pitch = target.angles.x * radians, yaw = target.angles.y * radians;
        Vector forward( std::cos( pitch ) * std::cos( yaw ), std::cos( pitch ) * std::sin( yaw ), -std::sin( pitch ) );
        points[10] = target.eye + forward * 64;
        points[11] = target.origin + Vector( 0, 0, 32 ) + forward * 40;
    }

    void BuildSamples( Snapshot& target )
    {
        FillSamples( target, settings.BoundsPadding, target.samples );
        FillSamples( target, 28, target.smokeSamples );
        target.samplesReady = true;
    }

    void PrepareViewerPrediction( Snapshot& viewer )
    {
        if ( viewer.eyeLeadReady )
            return;
        viewer.eyeLeadReady = true;
        viewer.visPeekCell = viewer.visCell;
        if ( !settings.CornerPrediction )
        {
            return;
        }
        const Vector wanted =
            CornerLead( viewer.velocity, viewer.angles.y, viewer.movementButtons, settings.CornerPredictionSeconds, settings.CornerMaxDistance );
        const float distance = wanted.Length();
        if ( distance > .01f && Finite( wanted ) && traceManager && traceFunction && traces < TraceBudget )
        {
            SightFilter filter( viewer, viewer );
            Ray_t ray;
            alignas( 16 ) CGameTrace result;
            Vector start = viewer.eye, end = start + wanted;
            ++traces;
            traceFunction( traceManager, ray, start, end, &filter, &result );
            viewer.eyeLead = wanted * CornerClipScale( distance, result.m_flFraction, result.m_bStartInSolid );
        }
        // Unknown/nonstandard VIS cells stay unknown. Apply the same clipped lead
        // to both the VIS lookup and later wall/smoke sample origins.
        if ( viewer.visCell >= 0 )
            viewer.visPeekCell = visibility.Cell( viewer.origin + viewer.eyeLead );
    }

    DirectVisibility CheckVisibility( int viewerSlot, int targetSlot )
    {
        auto& viewer = players[viewerSlot];
        auto& target = players[targetSlot];

        PrepareViewerPrediction( viewer );
        auto eyeLead = viewer.eyeLead;
        // The baker's dilation covers default wall padding. Larger custom bounds use engine rays.
        if ( visibility.BlockedCells( viewer.visCell, viewer.visPeekCell, target.visCell, target.visCell ) )
        {
            return DirectVisibility::Hidden;
        }

        if ( !target.samplesReady )
            BuildSamples( target );

        int count = eyeLead.LengthSqr() < 1 ? 12 : 24;
        int first = preferred[viewerSlot][targetSlot];

        if ( first >= count )
            first %= 12;

        // Verified smoke can hide independently of uncertain or exhausted wall rays.
        if ( IsSmokeHidden( viewer, target, eyeLead, count, first ) )
            return DirectVisibility::Hidden;

        if ( !traceManager || !traceFunction )
            return DirectVisibility::Unknown;

        SightFilter filter( viewer, target );

        for ( int order = 0; order < count; ++order )
        {
            if ( traces >= TraceBudget )
            {
                return DirectVisibility::Unknown;
            }

            int index = OrderedSample( order, first );
            bool prediction = index >= 12;

            Vector start = viewer.eye + ( prediction ? eyeLead : Vector( 0, 0, 0 ) );
            Vector end = target.samples[index % 12];

            if ( !Finite( start ) || !Finite( end ) )
            {
                return DirectVisibility::Unknown;
            }

            Ray_t ray;
            alignas( 16 ) CGameTrace result;
            ++traces;

            traceFunction( traceManager, ray, start, end, &filter, &result );

            if ( !Occluded( result.m_flFraction, result.m_bStartInSolid ) )
            {
                bool clear = !result.m_bStartInSolid && std::isfinite( result.m_flFraction ) && result.m_flFraction >= .99f && result.m_flFraction <= 1;

                if ( !clear )
                {
                    return DirectVisibility::Unknown;
                }
                preferred[viewerSlot][targetSlot] = index;
                return DirectVisibility::Visible;
            }

            if ( result.m_pEnt && !filter.ShouldHitEntity( result.m_pEnt ) )
            {
                return DirectVisibility::Unknown;
            }
        }

        return DirectVisibility::Hidden;
    }

    void EvaluateViewer( int slot, int tick )
    {
        if ( evaluated[slot] )
            return;

        evaluated[slot] = true;
        auto& viewer = players[slot];

        if ( !viewer.pawn || !viewer.human )
            return;

        for ( int order = 0; order < PlayerLimit; ++order )
        {
            int targetSlot = RotatedIndex( order, uint64_t( uint32_t( tick ) ) + slot, PlayerLimit );
            if ( !Enemy( slot, targetSlot, identities ) )
                continue;
            pairVisibility.Evaluate( slot, targetSlot, tick, settings.VisibleGraceTicks, deadlines[slot][targetSlot], deadlines[targetSlot][slot],
                                     [&]( int from, int to )
                                     {
                                         return players[to].complete ? CheckVisibility( from, to ) : DirectVisibility::Unknown;
                                     } );
        }
    }

    const std::vector<CEntityHandle>& GetHidden( int slot, int tick )
    {
        hidden[slot].clear();
        if ( !players[slot].pawn || !players[slot].human )
            return hidden[slot];
        for ( int targetSlot = 0; targetSlot < PlayerLimit; ++targetSlot )
        {
            if ( !Enemy( slot, targetSlot, identities ) || !pairVisibility.Hidden( slot, targetSlot, tick, deadlines[slot][targetSlot] ) )
                continue;
            auto& target = players[targetSlot];
            bool valid = true;

            for ( auto h : target.entities )
                if ( h.GetEntryIndex() <= 0 || h.GetEntryIndex() >= EntityLimit || !EntityAt( entitySystem, h ) )
                {
                    valid = false;
                    break;
                }
            if ( valid )
                hidden[slot].insert( hidden[slot].end(), target.entities.begin(), target.entities.end() );
        }

        return hidden[slot];
    }

  public:
    AntiWallHack() = default;

    bool Load( PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late ) override
    {
        PLUGIN_SAVEVARS();
        DebugStage( "Load: entered" );

        if ( !KHook::__exported__khook )
        {
            ismm->Format( error, maxlen, "Metamod 2.0 KHook API is required" );
            return false;
        }

        DebugStage( "Load: acquiring engine/server interfaces" );
        GET_V_IFACE_CURRENT( GetServerFactory, gameEntities, ISource2GameEntities, INTERFACEVERSION_SERVERGAMEENTS );
        GET_V_IFACE_CURRENT( GetServerFactory, serverDll, ISource2Server, INTERFACEVERSION_SERVERGAMEDLL );
        GET_V_IFACE_CURRENT( GetEngineFactory, network, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION );
        GET_V_IFACE_CURRENT( GetEngineFactory, resources, IGameResourceService, GAMERESOURCESERVICESERVER_INTERFACE_VERSION );
        GET_V_IFACE_CURRENT( GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION );
        ISchemaSystem* schema = static_cast<ISchemaSystem*>( ismm->GetEngineFactory()( SCHEMASYSTEM_INTERFACE_VERSION, nullptr ) );

        if ( !schema )
        {
            ismm->Format( error, maxlen, "SchemaSystem interface unavailable" );
            return false;
        }
        try
        {
            DebugStage( "Load: interfaces acquired" );
            const fs::path baseDir( ismm->GetBaseDir() );
            gamePath = baseDir;
            configPath = baseDir / "cfg/AntiWallHack.cfg";
            LoadSettings();
            DebugStage( "Load: configuration loaded" );
            auto data = ReadGamedata( baseDir / "addons/antiwallhack/gamedata.jsonc" );
            auto platform = data.at( "linux" );
            const char* module = "libserver.so";
            entitySystemOffset = platform.at( "GameEntitySystem" ).get<int>();

            if ( entitySystemOffset < 0 || entitySystemOffset > 1024 || entitySystemOffset % 8 )
                throw std::runtime_error( "Invalid GameEntitySystem offset" );

            auto* scope = schema->FindTypeScopeForModule( module );

            if ( !scope )
                throw std::runtime_error( "Server schema scope unavailable" );

            DebugStage( "Load: resolving schema offsets" );
            if ( !offsets.Init( scope ) )
            {
                ismm->Format( error, maxlen, "AntiWallHack: required schema fields unavailable, see console" );
                META_CONPRINTF( "[AntiWallHack] Startup rejected: required schema fields unavailable. No hooks installed.\n" );
                return false;
            }

            if ( settings.CornerPrediction )
            {
                offsets.movement = Offsets::Find( scope, "CBasePlayerPawn", "m_pMovementServices" );
                offsets.buttons = Offsets::Find( scope, "CPlayer_MovementServices", "m_nButtons" );
                if ( auto* globalScope = schema->GlobalTypeScope() )
                    offsets.buttonStates = Offsets::Find( globalScope, "CInButtonState", "m_pButtonStates" );
                if ( offsets.movement < 0 || offsets.buttons < 0 || offsets.buttonStates < 0 )
                    META_CONPRINTF( "[AntiWallHack] Movement input schema unavailable; corner prediction uses bounded velocity only.\n" );
            }

            int index = KHook::GetVtableIndex( &ISource2GameEntities::CheckTransmit );
            DebugStage( "Load: schema ready; resolving trace signature" );

            if ( index < 0 || index >= 100 )
                throw std::runtime_error( "Invalid CheckTransmit vtable index" );

            auto* serverFunction = ( *reinterpret_cast<void***>( gameEntities ) )[index];

            traceFunction = reinterpret_cast<TraceFunction>( FindServerFunction( platform.at( "TraceShape" ).get<std::string>(), serverFunction ) );
            if ( !traceFunction )
                throw std::runtime_error( "TraceShape signature missing or ambiguous; update gamedata.jsonc" );

            traceHook = std::make_unique<TraceHook>( this, &AntiWallHack::OnTrace, nullptr );
            DebugStage( "Load: configuring trace hook" );
            traceHook->Configure( traceFunction );
            transmitHook = std::make_unique<TransmitHook>( &ISource2GameEntities::CheckTransmit, this, nullptr, &AntiWallHack::OnTransmit );
            DebugStage( "Load: installing transmit hook" );
            transmitHook->Add( gameEntities );
            hooked = true;
            using FrameCallback = KHook::Return<void> ( AntiWallHack::* )( ISource2Server*, bool, bool, bool );
            frameHook = std::make_unique<FrameHook>( &ISource2Server::GameFrame, this, static_cast<FrameCallback>( nullptr ), &AntiWallHack::OnFrame );
            frameHook->Add( serverDll );
            ismm->AddListener( this, this );
            DebugStage( "Load: hooks installed; configuring convar" );
            ConfigureConvar();
            SetupSmoke( serverFunction, scope, platform );
            DebugStage( "Load: completed" );
            return true;
        }
        catch ( const std::exception& ex )
        {
            if ( frameHook )
            {
                frameHook->Remove( serverDll );
                frameHook.reset();
            }
            if ( hooked && transmitHook )
            {
                transmitHook->Remove( gameEntities );
                hooked = false;
            }

            RemoveEventHook();
            transmitHook.reset();
            traceHook.reset();

            META_CONPRINTF( "[AntiWallHack] Startup rejected: %s\n", ex.what() );
            ismm->Format( error, maxlen, "AntiWallHack: %s", ex.what() );

            return false;
        }
    }

    bool Unload( char*, size_t ) override
    {
        if ( frameHook )
        {
            frameHook->Remove( serverDll );
            frameHook.reset();
        }
        if ( visCancel )
            visCancel->store( true );
        DebugStage( "Unload: entered; waiting for VIS worker" );
        if ( visPending.valid() )
            visPending.wait();
        DebugStage( "Unload: VIS worker finished; removing hooks" );
        if ( hooked && transmitHook )
        {
            transmitHook->Remove( gameEntities );
            hooked = false;
        }

        RemoveEventHook();
        smokeAvailable = false;
        smokeVolumes.clear();
        smokeExplosions.Reset();
        transmitHook.reset();
        traceHook.reset();
        ConVarRefAbstract cvar( "sv_enable_donttransmit" );

        if ( convarChanged && cvar.IsValidRef() && !cvar.GetBool() )
            cvar.SetString( previousConvar.c_str() );

        convarChanged = false;
        visibility = {};
        visMapName.clear();
        traceManager = nullptr;
        entitySystem = nullptr;

        Reset();

        DebugStage( "Unload: completed" );
        return true;
    }

    void OnLevelInit( const char* map, const char*, const char*, const char*, bool, bool ) override
    {
        DebugStage( "OnLevelInit: entered" );
        debugTraceSeen.store( false );
        debugTransmitSeen = debugTransmitReady = debugFrameSeen = false;
        LoadVisibility( map, false );
        DebugStage( "OnLevelInit: VIS deferred; resetting frame state" );
        Reset();
        smokeExplosions.Reset();
        smokeCaptured = false;
        smokeVolumes.clear();
        traceManager = nullptr;
        entitySystem = nullptr;
        DebugStage( "OnLevelInit: configuring convar" );
        ConfigureConvar();
        DebugStage( "OnLevelInit: completed" );
    }

    void OnLevelShutdown() override
    {
        if ( visCancel )
            visCancel->store( true );
        DebugStage( "OnLevelShutdown: entered" );
        ++visGeneration;
        visibility = {};
        visMapName.clear();
        Reset();
        smokeExplosions.Reset();
        smokeCaptured = false;
        smokeVolumes.clear();
        traceManager = nullptr;
        entitySystem = nullptr;
        DebugStage( "OnLevelShutdown: completed" );
    }

    KHook::Return<bool> OnGameEvent( IGameEventManager2*, IGameEvent* event, bool )
    {
        if ( !smokeAvailable || !event || !visibility.Ready() )
            return { KHook::Action::Ignore };
        try
        {
            if ( std::strcmp( event->GetName(), "hegrenade_detonate" ) == 0 )
            {
                auto* server = network->GetIGameServer();
                auto* globals = server ? server->GetGlobals() : nullptr;
                if ( !globals )
                {
                    DisableSmoke( "HE event time unavailable" );
                    return { KHook::Action::Ignore };
                }
                if ( !event->HasKey( "x" ) || !event->HasKey( "y" ) || !event->HasKey( "z" ) )
                {
                    DisableSmoke( "HE event coordinates unavailable" );
                    return { KHook::Action::Ignore };
                }
                smoke::Point center{ event->GetFloat( "x" ), event->GetFloat( "y" ), event->GetFloat( "z" ) };
                if ( !smoke::Finite( center ) || !std::isfinite( globals->curtime ) )
                {
                    DisableSmoke( "invalid HE event position or time" );
                    return { KHook::Action::Ignore };
                }
                smokeExplosions.Add( center, globals->curtime );
                smokeCaptured = false;
                smokeVolumes.clear();
                // Never reuse a smoke decision after a new opening in the same tick.
                evaluated.fill( false );
                pairVisibility.Reset();
                for ( auto& row : hidden )
                    row.clear();
            }
        }
        catch ( const std::exception& ex )
        {
            DisableSmoke( ex.what() );
        }
        return { KHook::Action::Ignore };
    }

    KHook::Return<void> OnTrace( void* manager, Ray_t&, Vector&, Vector&, CTraceFilter*, CGameTrace* )
    {
        if ( settings.Debug && !debugTraceSeen.exchange( true ) )
            DebugStage( "OnTrace: first engine trace callback" );
        traceManager = manager;
        return { KHook::Action::Ignore };
    }

    KHook::Return<void> OnTransmit( ISource2GameEntities*, CCheckTransmitInfo** infos, int count, CBitVec<16384>&, CBitVec<16384>&,
                                    const Entity2Networkable_t**, const uint16*, int )
    {
        if ( !debugTransmitSeen )
        {
            debugTransmitSeen = true;
            DebugStage( "OnTransmit: first callback" );
        }
        if ( faulted || !settings.Enabled || !infos || count < 1 || count > 64 )
            return { KHook::Action::Ignore };
        try
        {
            auto* server = network->GetIGameServer();
            auto* globals = server ? server->GetGlobals() : nullptr;
            // CheckTransmit can run during loading before the world/trace manager
            // is ready. Do not query map/addon state until the original readiness
            // prerequisites hold.
            if ( !server || !globals || !traceManager )
            {
                Reset();
                return { KHook::Action::Ignore };
            }

            bool tracing = settings.Debug && !debugTransmitReady;
            if ( tracing )
                DebugStage( "OnTransmit: readiness guard passed; GetAddonName" );
            const char* addon = server->GetAddonName();
            if ( tracing )
                DebugStage( "OnTransmit: GetAddonName returned; GetMapName" );
            const char* activeMap = server->GetMapName();
            if ( tracing )
                DebugStage( "OnTransmit: GetMapName returned; resolving VIS identity" );
            auto identity = MapIdentity( activeMap ? activeMap : "", addon ? addon : "" );
            if ( identity.empty() )
            {
                visibility = {};
                if ( !visMapName.empty() )
                {
                    visMapName.clear();
                    ++visGeneration;
                    visObserved = false;
                }
            }
            else if ( visMapName != identity )
                LoadVisibility( identity.c_str() );
            else
                UpdateVisibility();
            if ( tracing )
            {
                debugTransmitReady = true;
                DebugStage( "OnTransmit: VIS update returned" );
            }
            NotifyMissingVisibility();
            if ( !visibility.Ready() )
            {
                Reset();
                return { KHook::Action::Ignore };
            }
            entitySystem = Field<CGameEntitySystem*>( resources, entitySystemOffset );
            ConfigureConvar();
            ConVarRefAbstract cvar( "sv_enable_donttransmit" );
            if ( !globals || !entitySystem || !traceManager || ( cvar.IsValidRef() && cvar.GetBool() ) || !RoundActive() )
            {
                Reset();
                return { KHook::Action::Ignore };
            }
            if ( !debugFrameSeen )
                DebugStage( "OnTransmit: capturing first player frame" );
            BeginFrame( globals->tickcount, globals->curtime );
            if ( !debugFrameSeen )
            {
                debugFrameSeen = true;
                DebugStage( "OnTransmit: first player frame captured" );
            }
            // Establish all shared holds in this batch before touching any masks.
            for ( int i = 0; i < count; ++i )
            {
                int index = RotatedIndex( i, uint32_t( globals->tickcount ), count );
                auto* info = reinterpret_cast<TransmitInfo*>( infos[index] );
                if ( !info || info->fullUpdate || !info->entities || !info->deletion || info->entities == info->deletion )
                    continue;
                const int slot = info->slot.Get();
                if ( slot >= 0 && slot < PlayerLimit )
                    EvaluateViewer( slot, globals->tickcount );
            }
            for ( int i = 0; i < count; ++i )
            {
                int index = RotatedIndex( i, uint32_t( globals->tickcount ), count );
                auto* info = reinterpret_cast<TransmitInfo*>( infos[index] );

                if ( !info || info->fullUpdate || !info->entities || !info->deletion || info->entities == info->deletion )
                    continue;

                int slot = info->slot.Get();

                if ( slot < 0 || slot >= 64 )
                    continue;

                for ( auto h : GetHidden( slot, globals->tickcount ) )
                    if ( EntityAt( entitySystem, h ) )
                        Withhold( info->entities->Base(), info->deletion->Base(), h.GetEntryIndex() );
            }
        }
        catch ( const std::exception& ex )
        {
            faulted = true;
            Reset();
            META_CONPRINTF( "[AntiWallHack] Filtering disabled after error: %s\n", ex.what() );
        }

        return { KHook::Action::Ignore };
    }

    const char* GetAuthor() override
    {
        return "M1K@c";
    }

    const char* GetName() override
    {
        return "AntiWallHack";
    }

    const char* GetDescription() override
    {
        return "Server-side enemy visibility filtering for CS2";
    }

    const char* GetURL() override
    {
        return "https://discord.gg/d5uvMmUpuE";
    }

    const char* GetLicense() override
    {
        return "AGPL-3.0";
    }

    const char* GetVersion() override
    {
        return "1.0.3";
    }

    const char* GetDate() override
    {
        return __DATE__;
    }

    const char* GetLogTag() override
    {
        return "AntiWallHack";
    }
};

AntiWallHack g_AntiWallHack;
PLUGIN_EXPOSE( AntiWallHack, g_AntiWallHack );
