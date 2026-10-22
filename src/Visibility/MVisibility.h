#pragma once
#include <array>
#include <cstdint>
#include "Helpers/VisibilityRules.h"

namespace awh
{
    enum class DirectVisibility : uint8_t
    {
        Unchecked,
        Hidden,
        Visible,
        Unknown,
        Held
    };

    // Only fresh positive visibility propagates. Grace and fail-open uncertainty
    class PairVisibilityCache
    {
        std::array<std::array<DirectVisibility, PlayerLimit>, PlayerLimit> values{};

      public:
        void Reset()
        {
            for ( auto& row : values )
            {
                row.fill( DirectVisibility::Unchecked );
            }
        }

        template <class Query> void Evaluate( int viewer, int target, int tick, int grace, int& deadline, int& reverseDeadline, Query query )
        {
            auto& value = values[viewer][target];

            if ( value != DirectVisibility::Unchecked )
            {
                return;
            }

            if ( !CanHide( tick, deadline ) )
            {
                value = DirectVisibility::Held;
                return;
            }

            value = query( viewer, target );

            if ( value == DirectVisibility::Visible )
            {
                deadline = std::max( deadline, tick + grace );
                reverseDeadline = std::max( reverseDeadline, tick + grace );
            }
            else if ( value == DirectVisibility::Unknown )
            {
                deadline = std::max( deadline, tick + grace ); // Preserve directional fail-open hold only.
            }
        }

        bool Hidden( int viewer, int target, int tick, int deadline ) const
        {
            // Read after all recipients in this batch have been evaluated: a later
            // positive reveal can cancel an earlier hidden decision through grace.
            return values[viewer][target] == DirectVisibility::Hidden && CanHide( tick, deadline );
        }
    };
}
