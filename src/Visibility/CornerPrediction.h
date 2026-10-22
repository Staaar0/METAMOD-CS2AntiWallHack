#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace awh
{
    template <class Point> Point CornerLead( Point velocity, float yaw, uint64_t buttons, float seconds, float maximum )
    {
        if ( seconds <= 0 || maximum <= 0 )
        {
            return Point{ 0, 0, 0 };
        }

        Point lead{ velocity.x * seconds, velocity.y * seconds, velocity.z * seconds };
        const int forward = int( bool( buttons & 0x8 ) ) - int( bool( buttons & 0x10 ) );
        const int side = int( bool( buttons & 0x400 ) ) - int( bool( buttons & 0x200 ) );

        if ( forward || side )
        {
            const float radians = yaw * .01745329251994329577f;
            const float x = std::cos( radians ) * forward + std::sin( radians ) * side;
            const float y = std::sin( radians ) * forward - std::cos( radians ) * side;
            const float length = std::sqrt( x * x + y * y );
            const float distance = std::min( maximum, std::max( std::min( 16.0f, maximum ), std::sqrt( lead.x * lead.x + lead.y * lead.y ) ) );
            lead.x = x / length * distance;
            lead.y = y / length * distance;
        }

        const float length = std::sqrt( lead.x * lead.x + lead.y * lead.y + lead.z * lead.z );

        if ( length > maximum && length > 0 )
        {
            lead.x *= maximum / length;
            lead.y *= maximum / length;
            lead.z *= maximum / length;
        }

        return lead;
    }

    // A wall hit keeps the predicted eye two units in front of the surface.
    inline float CornerClipScale( float distance, float fraction, bool solid )
    {
        if ( !std::isfinite( distance ) || distance <= 0 || solid || !std::isfinite( fraction ) || fraction < 0 || fraction > 1 )
        {
            return 0;
        }

        if ( fraction == 1 )
        {
            return 1;
        }

        return std::max( 0.0f, fraction - 2.0f / distance );
    }
}
