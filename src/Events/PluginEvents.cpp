#include "Plugin.h"

KHook::Return<void> AntiWallHack::OnFrame( ISource2Server*, bool, bool, bool )
{
    // client's CheckTransmit callback. Keep empty servers prepared as well.
    if ( settings.Enabled && traceManager && !visMapName.empty() )
    {
        UpdateVisibility();
    }

    PollUpdates();
    NotifyMissingVisibility();
    return { KHook::Action::Ignore };
}

void AntiWallHack::OnLevelInit( const char* map, const char*, const char*, const char*, bool, bool )
{
    DebugStage( "OnLevelInit: entered" );
    debugTraceSeen.store( false );
    debugTransmitSeen = debugTransmitReady = debugFrameSeen = false;
    LoadVisibility( map, false );
    DebugStage( "OnLevelInit: VIS deferred, resetting frame state" );
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

void AntiWallHack::OnLevelShutdown()
{
    if ( visCancel )
    {
        visCancel->store( true );
    }

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

KHook::Return<bool> AntiWallHack::OnGameEvent( IGameEventManager2*, IGameEvent* event, bool )
{
    if ( !smokeAvailable || !event || !visibility.Ready() )
    {
        return { KHook::Action::Ignore };
    }

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
            {
                row.clear();
            }
        }
    }
    catch ( const std::exception& ex )
    {
        DisableSmoke( ex.what() );
    }

    return { KHook::Action::Ignore };
}
