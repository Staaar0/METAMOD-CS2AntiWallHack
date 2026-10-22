#include "Plugin.h"

KHook::Return<void> AntiWallHack::OnTransmit( ISource2GameEntities*, CCheckTransmitInfo** infos, int count, CBitVec<16384>&, CBitVec<16384>&, const Entity2Networkable_t**,
                                              const uint16*, int )
{
    if ( !debugTransmitSeen )
    {
        debugTransmitSeen = true;
        DebugStage( "OnTransmit: first callback" );
    }

    if ( faulted || !settings.Enabled || !infos || count < 1 || count > 64 )
    {
        return { KHook::Action::Ignore };
    }

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
        {
            DebugStage( "OnTransmit: readiness guard passed; GetAddonName" );
        }

        const char* addon = server->GetAddonName();

        if ( tracing )
        {
            DebugStage( "OnTransmit: GetAddonName returned; GetMapName" );
        }

        const char* activeMap = server->GetMapName();

        if ( tracing )
        {
            DebugStage( "OnTransmit: GetMapName returned; resolving VIS identity" );
        }

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
        {
            LoadVisibility( identity.c_str() );
        }
        else
        {
            UpdateVisibility();
        }

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
        {
            DebugStage( "OnTransmit: capturing first player frame" );
        }

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
            {
                continue;
            }

            const int slot = info->slot.Get();

            if ( slot >= 0 && slot < PlayerLimit )
            {
                EvaluateViewer( slot, globals->tickcount );
            }
        }

        for ( int i = 0; i < count; ++i )
        {
            int index = RotatedIndex( i, uint32_t( globals->tickcount ), count );
            auto* info = reinterpret_cast<TransmitInfo*>( infos[index] );

            if ( !info || info->fullUpdate || !info->entities || !info->deletion || info->entities == info->deletion )
            {
                continue;
            }

            int slot = info->slot.Get();

            if ( slot < 0 || slot >= 64 )
            {
                continue;
            }

            for ( auto h : GetHidden( slot, globals->tickcount ) )
            {
                if ( EntityAt( entitySystem, h ) )
                {
                    Withhold( info->entities->Base(), info->deletion->Base(), h.GetEntryIndex() );
                }
            }
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
