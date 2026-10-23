#pragma once
#include "Helpers/Entities.h"

namespace awh
{
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
                if ( !Readable( info, sizeof( *info ) ) || info->m_nNumFields > 4096 || info->m_nNumBaseClasses > 32 ||
                     ( info->m_nNumFields && !Readable( info->m_pFields, size_t( info->m_nNumFields ) * sizeof( SchemaClassFieldData_t ) ) ) )
                {
                    META_CONPRINTF( "[AntiWallHack] Invalid schema metadata: %s\n", cls );
                    return -1;
                }

                for ( int i = 0; i < info->m_nNumFields; ++i )
                {
                    if ( SchemaNameEquals( info->m_pFields[i].m_pName, name ) )
                    {
                        int offset = info->m_pFields[i].m_nSingleInheritanceOffset;

                        if ( offset < 0 || offset >= info->m_nSizeOf )
                        {
                            META_CONPRINTF( "[AntiWallHack] Invalid schema offset: %s::%s\n", cls, name );
                            return -1;
                        }

                        return offset;
                    }
                }

                if ( info->m_nNumBaseClasses && !Readable( info->m_pBaseClasses, size_t( info->m_nNumBaseClasses ) * sizeof( SchemaBaseClassInfoData_t ) ) )
                {
                    META_CONPRINTF( "[AntiWallHack] Invalid schema base metadata: %s\n", cls );
                    return -1;
                }

                info = info->m_nNumBaseClasses ? info->m_pBaseClasses[0].m_pClass : nullptr;
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

            for ( int value : { pawn,   currentPawn, pawnController, spawning, connected, flags, hltv, team,  health,  life,          body,  scene,  collision, velocity, view,
                                angles, origin,      child,          sibling,  nodeOwner, mins,  maxs, owner, weapons, weaponHandles, rules, warmup, freeze,    winner } )
            {
                if ( value < 0 )
                {
                    return false;
                }
            }

            return true;
        }
    };

}
