#pragma once
#include <ISmmPlugin.h>
#include <gameentitysystem.h>
#include <gametrace.h>
#include <schemasystem/schemasystem.h>
#include "Helpers/Memory.h"

PLUGIN_GLOBALVARS();

namespace awh
{
    template <class T> T& Field( void* object, int offset )
    {
        return *reinterpret_cast<T*>( static_cast<uint8_t*>( object ) + offset );
    }

    inline CEntityInstance* EntityAt( CGameEntitySystem* system, CEntityIndex index )
    {
        const int value = index.Get();

        if ( !system || value < 0 || value >= MAX_TOTAL_ENTITIES - 1 )
        {
            return nullptr;
        }

        auto* chunk = system->m_EntityList.m_pIdentityChunks[value / MAX_ENTITIES_IN_LIST];

        if ( !chunk )
        {
            return nullptr;
        }

        auto& identity = chunk[value % MAX_ENTITIES_IN_LIST];

        return identity.GetEntityIndex() == index ? identity.m_pInstance : nullptr;
    }

    inline CEntityInstance* EntityAt( CGameEntitySystem* system, const CEntityHandle& handle )
    {
        if ( !handle.IsValid() )
        {
            return nullptr;
        }

        auto* entity = EntityAt( system, CEntityIndex( handle.GetEntryIndex() ) );

        return entity && entity->GetRefEHandle() == handle ? entity : nullptr;
    }

    inline bool Finite( const Vector& v )
    {
        return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z );
    }

    inline bool IsClass( CEntityInstance* entity, const char* name )
    {
        if ( !entity || !entity->m_pEntity )
        {
            return false;
        }

        auto* info = entity->Schema_DynamicBinding().m_pObj;

        for ( int depth = 0; info && depth < 32; ++depth )
        {
            if ( !std::strcmp( info->m_pName, name ) )
            {
                return true;
            }

            info = info->m_nNumBaseClasses ? info->m_pBaseClasses[0].m_pClass : nullptr;
        }

        return false;
    }

}
