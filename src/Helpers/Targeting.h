#pragma once
#include "Helpers/VisibilityRules.h"

namespace awh
{
    inline int RotatedIndex( int order, uint64_t phase, int count )
    {
        return count > 0 ? ( order + int( phase % unsigned( count ) ) ) % count : -1;
    }

    template <class Roster> bool Enemy( int viewer, int target, const Roster& roster )
    {
        if ( viewer < 0 || viewer >= PlayerLimit || target < 0 || target >= PlayerLimit || viewer == target )
        {
            return false;
        }

        const auto& a = roster[viewer];
        const auto& b = roster[target];

        return a.pawn && a.controller && b.pawn && b.controller && ( a.team == 2 || a.team == 3 ) && ( b.team == 2 || b.team == 3 ) && a.team != b.team;
    }
}
