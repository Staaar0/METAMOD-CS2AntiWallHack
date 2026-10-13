#pragma once
#include "rules.h"
#include <array>

namespace awh
{
    inline int RotatedIndex( int order, uint64_t phase, int count )
    {
        return count > 0 ? ( order + int( phase % unsigned( count ) ) ) % count : -1;
    }

    // Cache
    struct NearestTargets
    {
        std::array<int, PlayerLimit> slots{};
        int count = 0;
        float refreshedAt = 0;
        uint64_t rosterRevision = 0;
        int limit = 0;
        bool ready = false;

        template <class Roster, class Distance> void Update( float now, uint64_t revision, int capacity, int viewer, const Roster& roster, Distance distance )
        {
            if ( !std::isfinite( now ) || viewer < 0 || viewer >= PlayerLimit || !roster[viewer].pawn || !roster[viewer].controller )
            {
                *this = {};
                return;
            }

            capacity = std::clamp( capacity, 0, PlayerLimit - 1 );
            
            if ( ready && revision == rosterRevision && capacity == limit && now >= refreshedAt && now - refreshedAt < 1.f )
                return;

            count = 0;
            float distances[PlayerLimit];

            for ( int slot = 0; slot < PlayerLimit; ++slot )
            {
                const auto& identity = roster[slot];
                if ( slot == viewer || !identity.pawn || !identity.controller || ( identity.team != 2 && identity.team != 3 ) ||
                    identity.team == roster[viewer].team )
                    continue;

                InsertNearest( slots.data(), distances, count, capacity, slot, distance( slot ) );
            }

            refreshedAt = now;
            rosterRevision = revision;
            limit = capacity;
            ready = true;
        }
    };
}
