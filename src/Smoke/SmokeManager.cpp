#include "Plugin.h"

void AntiWallHack::DisableSmoke( const std::string& reason )
{
    smokeAvailable = false;
    smokeVolumes.clear();
    evaluated.fill( false );
    pairVisibility.Reset();

    for ( auto& row : hidden )
    {
        row.clear();
    }

    META_CONPRINTF( "[AntiWallHack] Smoke filtering disabled until plugin reload: %s. Wall filtering continues.\n", reason.c_str() );
}

void AntiWallHack::RemoveEventHook()
{
    if ( eventHooked && eventHook && eventManager )
    {
        eventHook->Remove( eventManager );
    }

    eventHooked = false;
    eventHook.reset();
    eventManager = nullptr;
}

void AntiWallHack::SetupSmoke( void* serverFunction, CSchemaSystemTypeScope* scope, const Json& platform )
{
    DebugStage( "Smoke: setup entered" );

    if ( !settings.SmokeOcclusion )
    {
        META_CONPRINTF( "[AntiWallHack] Smoke filtering disabled by SmokeOcclusion 0.\n" );
        return;
    }

    std::string reason;
    std::vector<smoke::Fingerprint> fingerprints;

    try
    {
        smokeLayout = SmokeLayout( platform );
        smokeFrame.resize( smokeLayout.frameBytes );

        for ( const auto& fingerprint : platform.at( "ServerBinaries" ) )
        {
            fingerprints.push_back( { fingerprint.at( "size" ).get<uint64_t>(), fingerprint.at( "crc" ).get<uint32_t>() } );
        }
    }
    catch ( const std::exception& ex )
    {
        DisableSmoke( ex.what() );
        return;
    }

    DebugStage( "Smoke: verifying server fingerprint" );

    if ( !smoke::VerifyModule( serverFunction, fingerprints, reason ) )
    {
        DisableSmoke( reason );
        return;
    }

    try
    {
        DebugStage( "Smoke: fingerprint accepted, resolving schema/event manager" );
        smokeEffectOffset = Offsets::Find( scope, "CSmokeGrenadeProjectile", "m_bDidSmokeEffect" );

        if ( smokeEffectOffset < 0 )
        {
            throw std::runtime_error( "smoke schema field unavailable" );
        }

        auto* signature = static_cast<uint8_t*>( FindServerFunction( platform.at( "GameEventManager" ).get<std::string>(), serverFunction ) );
        int32_t displacement = 0;

        if ( !signature || !smoke::ReadValue( reinterpret_cast<uintptr_t>( signature ) + 3, displacement ) )
        {
            throw std::runtime_error( "HE event manager signature unavailable" );
        }

        auto address = reinterpret_cast<uintptr_t>( signature ) + 7 + displacement;
        eventManagerAddress = address;

        if ( !smoke::ReadValue( address, eventManager ) || !eventManager )
        {
            throw std::runtime_error( "HE event manager unavailable" );
        }

        void** vtable = nullptr;
        int index = platform.at( "FireEvent" ).get<int>();
        void* function = nullptr;

        if ( index != KHook::GetVtableIndex( &IGameEventManager2::FireEvent ) || !smoke::ReadValue( reinterpret_cast<uintptr_t>( eventManager ), vtable ) || !vtable ||
             !smoke::ReadValue( reinterpret_cast<uintptr_t>( vtable + index ), function ) || !function )
        {
            throw std::runtime_error( "HE event dispatch layout mismatch" );
        }

        eventHook = std::make_unique<EventHook>( &IGameEventManager2::FireEvent, this, &AntiWallHack::OnGameEvent, nullptr );
        DebugStage( "Smoke: installing HE event hook" );
        eventHook->Add( eventManager );
        eventHooked = true;

        if ( !eventHook->Installed( vtable, index ) )
        {
            throw std::runtime_error( "HE event hook installation failed" );
        }

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

void AntiWallHack::CaptureSmoke()
{
    smokeCaptured = true;
    smokeVolumes.clear();

    if ( !smokeAvailable || !smokeExplosions.Begin( frameTime ) )
    {
        return;
    }

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
            {
                continue;
            }

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
            {
                continue;
            }

            if ( count == smokeStorage.size() )
            {
                smokeVolumes.clear();
                return;
            }

            auto& volume = smokeStorage[count];

            if ( !volume )
            {
                volume = std::make_unique<smoke::Volume>();
            }

            if ( !smoke::ReadSnapshot( reinterpret_cast<uintptr_t>( entity ), smokeLayout, frameTime, *volume, smokeFrame, smoke::ReadMemory ) )
            {
                // An unpublished/unstable voxel frame only invalidates this tick.
                smokeVolumes.clear();
                auto now = std::chrono::steady_clock::now();

                if ( now >= smokeNextWarning )
                {
                    smokeNextWarning = now + std::chrono::seconds( 30 );
                    META_CONPRINTF( "[AntiWallHack] Smoke snapshot unavailable, revealing this tick and retrying. Wall "
                                    "filtering continues.\n" );
                }

                return;
            }

            ++count;

            if ( !smokeExplosions.Clears( *volume, frameTime ) )
            {
                smokeVolumes.push_back( volume.get() );
            }
        }
    }
    catch ( const std::exception& ex )
    {
        DisableSmoke( ex.what() );
    }
}

bool AntiWallHack::IsSmokeHidden( const Snapshot& viewer, const Snapshot& target, Vector eyeLead, int count, int first )
{
    if ( !smokeAvailable || !settings.SmokeOcclusion )
    {
        return false;
    }

    if ( !smokeCaptured )
    {
        CaptureSmoke();
    }

    if ( !smokeAvailable || smokeVolumes.empty() )
    {
        return false;
    }

    return smoke::SamplesBlocked( smokeVolumes, count, first, settings.CornerPrediction ? settings.CornerPredictionSeconds : 0, smokeCellBudget,
                                  [&]( int index )
                                  {
                                      bool predicted = index >= 12;

                                      Vector start = viewer.eye + ( predicted ? eyeLead : Vector( 0, 0, 0 ) );
                                      Vector end = target.smokeSamples[index % 12];

                                      return std::make_pair( smoke::Point{ start.x, start.y, start.z }, smoke::Point{ end.x, end.y, end.z } );
                                  } );
}
