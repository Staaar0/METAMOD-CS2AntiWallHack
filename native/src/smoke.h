#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include <sstream>
#include "rules.h"

namespace awh::smoke
{
    struct Point
    {
        float x = 0, y = 0, z = 0;

        float operator[]( int axis ) const
        {
            return axis == 0 ? x : axis == 1 ? y : z;
        }

        Point operator+( Point b ) const
        {
            return { x + b.x, y + b.y, z + b.z };
        }

        Point operator-( Point b ) const
        {
            return { x - b.x, y - b.y, z - b.z };
        }

        Point operator*( float n ) const
        {
            return { x * n, y * n, z * n };
        }

        float LengthSquared() const
        {
            return x * x + y * y + z * z;
        }
    };

    inline bool Finite( Point p )
    {
        return std::isfinite( p.x ) && std::isfinite( p.y ) && std::isfinite( p.z );
    }

    inline int Morton( int x, int y, int z )
    {
        int result = 0;
        for ( int bit = 0; bit < 5; ++bit )
            result |= ( ( x >> bit ) & 1 ) << ( bit * 3 ) | ( ( y >> bit ) & 1 ) << ( bit * 3 + 1 ) | ( ( z >> bit ) & 1 ) << ( bit * 3 + 2 );

        return result;
    }

    inline float Smooth( float x )
    {
        x = std::clamp( x, 0.f, 1.f );
        return x * x * ( 3 - 2 * x );
    }

    inline float AgeScale( float age )
    {
        return std::isfinite( age ) ? Smooth( ( age - .6f ) / 1.4f ) * Smooth( ( 21.5f - age ) / 5 ) : 0;
    }

    struct Volume
    {
        Point center;
        float age = 0, startTime = 0;
        std::array<uint8_t, 4096> opaque{};
        std::array<float, 32768> density{};
    };

    inline bool Clip( Point center, Point origin, Point direction, float& entry, float& exit )
    {
        entry = 0;
        exit = 1;

        if ( !Finite( center ) || !Finite( origin ) || !Finite( direction ) )
            return false;

        for ( int axis = 0; axis < 3; ++axis )
        {
            float lo = center[axis] - 320, hi = center[axis] + 320, d = direction[axis], p = origin[axis];
            if ( std::abs( d ) < 1e-7f )
            {
                if ( p < lo || p > hi )
                    return false;

                continue;
            }

            float a = ( lo - p ) / d, b = ( hi - p ) / d;
            entry = std::max( entry, std::min( a, b ) );
            exit = std::min( exit, std::max( a, b ) );

            if ( entry >= exit )
                return false;
        }
        return true;
    }

    inline float Contribution( const Volume& volume, Point origin, Point target, float advance )
    {
        auto direction = target - origin;

        if ( !Finite( origin ) || !Finite( target ) || !Finite( direction ) || !std::isfinite( advance ) )
            return 0;

        float scale = AgeScale( volume.age + std::max( advance, 0.f ) ), entry, exit;

        if ( scale <= 0 || direction.LengthSquared() < 1e-8f || !Clip( volume.center, origin, direction, entry, exit ) )
            return 0;

        auto minimum = volume.center - Point{ 320, 320, 320 };
        float t = entry;

        if ( entry > 0 )
            t += std::min( 1 / std::sqrt( direction.LengthSquared() ), exit - entry );

        auto start = origin + direction * t;
        int cell[3], step[3];
        float next[3], delta[3];

        for ( int axis = 0; axis < 3; ++axis )
        {
            cell[axis] = std::clamp( int( std::floor( ( start[axis] - minimum[axis] ) / 20 ) ), 0, 31 );
            float d = direction[axis];
            step[axis] = ( d > 0 ) - ( d < 0 );
            next[axis] = d == 0 ? std::numeric_limits<float>::infinity() : t + ( minimum[axis] + ( cell[axis] + ( d > 0 ? 1 : 0 ) ) * 20 - start[axis] ) / d;
            delta[axis] = d == 0 ? std::numeric_limits<float>::infinity() : 20 / std::abs( d );
        }

        float accumulated = 0;

        for ( int visited = 0; visited < 128; ++visited )
        {
            if ( unsigned( cell[0] ) >= 32 || unsigned( cell[1] ) >= 32 || unsigned( cell[2] ) >= 32 )
                break;

            int index = Morton( cell[0], cell[1], cell[2] );

            if ( volume.opaque[index >> 3] & ( 1 << ( index & 7 ) ) )
                return scale;

            float density = std::clamp( volume.density[index] / 50, 0.f, 1.f );

            if ( density >= .8f )
                return scale;

            if ( density > .1f )
            {
                accumulated += density;
                if ( accumulated >= .2f )
                    return scale;
            }

            float crossing = std::min( next[0], std::min( next[1], next[2] ) );

            if ( crossing >= exit )
                break;

            for ( int axis = 0; axis < 3; ++axis )
                if ( next[axis] == crossing )
                {
                    cell[axis] += step[axis];
                    next[axis] += delta[axis];
                }
        }

        return accumulated * scale;
    }

    struct Explosion
    {
        Point center;
        float time;
    };

    inline bool Cleared( const Volume& volume, const Explosion& explosion, float now )
    {
        if ( !Finite( explosion.center ) || !std::isfinite( explosion.time ) || !std::isfinite( now ) || explosion.time < volume.startTime ||
            now < explosion.time || now - explosion.time > 3 )
            return false;

        Point difference;

        difference.x = explosion.center.x - std::clamp( explosion.center.x, volume.center.x - 320, volume.center.x + 320 );
        difference.y = explosion.center.y - std::clamp( explosion.center.y, volume.center.y - 320, volume.center.y + 320 );
        difference.z = explosion.center.z - std::clamp( explosion.center.z, volume.center.z - 320, volume.center.z + 320 );

        return difference.LengthSquared() <= 10000;
    }

    class Explosions
    {
        std::vector<Explosion> events;
        float previousTime = 0, disabledUntil = 0;

    public:
        void Reset()
        {
            events.clear();
            previousTime = disabledUntil = 0;
        }

        void Add( Point center, float time )
        {
            if ( !Finite( center ) || !std::isfinite( time ) )
                return;

            if ( events.size() >= 64 )
            {
                events.clear();
                disabledUntil = time + 3;
            }

            events.push_back( { center, time } );
        }

        bool Begin( float now )
        {
            if ( !std::isfinite( now ) )
                return false;

            if ( now < previousTime )
                Reset();

            previousTime = now;
            events.erase( std::remove_if( events.begin(), events.end(), [&]( const Explosion& e ) { return now < e.time || now - e.time > 3; } ), events.end() );
            return now >= disabledUntil;
        }

        bool Clears( const Volume& volume, float now ) const
        {
            for ( const auto& e : events )
                if ( Cleared( volume, e, now ) )
                    return true;

            return false;
        }
    };

    inline constexpr size_t FrameBytes = 0x80000;

    template <class T> T Decode( const uint8_t* bytes, size_t offset )
    {
        T value;
        std::memcpy( &value, bytes + offset, sizeof( value ) );
        return value;
    }

    template <class Reader> bool ReadSnapshot( uintptr_t entity, size_t volumeOffset, float now, Volume& output, std::vector<uint8_t>& frame, Reader read )
    {
        if ( !entity || volumeOffset > uintptr_t( INTPTR_MAX ) - 240 || entity > uintptr_t( INTPTR_MAX ) - volumeOffset - 240 || !std::isfinite( now ) ||
            frame.size() != FrameBytes )
            return false;

        std::array<uint8_t, 240> before{}, after{};
        for ( int attempt = 0; attempt < 2; ++attempt )
        {
            if ( !read( entity + volumeOffset, before.data(), before.size() ) )
                return false;

            auto storage = Decode<uintptr_t>( before.data(), 112 );
            int active = Decode<int>( before.data(), 236 );
            float started = Decode<float>( before.data(), 12 );
            Point center = Decode<Point>( before.data(), 212 );
            float age = now - started;

            if ( ( active != 0 && active != 1 ) || !storage || storage > uintptr_t( INTPTR_MAX ) - 0x103008 || !Finite( center ) || !std::isfinite( age ) ||
                age < -.25f || age > 30 )
                return false;

            if ( !read( storage + 8, output.opaque.data(), output.opaque.size() ) || !read( storage + 0x3008 + active * FrameBytes, frame.data(), FrameBytes ) ||
                !read( entity + volumeOffset, after.data(), after.size() ) )
                return false;

            if ( before != after )
                continue;

            for ( size_t i = 0; i < output.density.size(); ++i )
            {
                float density = Decode<float>( frame.data(), i * 0x10 );
                if ( !std::isfinite( density ) || density < 0 )
                    return false;

                output.density[i] = density;
            }

            output.center = center;
            output.age = age;
            output.startTime = started;

            return true;
        }

        return false;
    }

    inline bool SupportedSize( uint64_t size )
    {
        return size == 41724600 || size == 41726328;
    }

    inline bool Matches( uint64_t size, uint32_t crc )
    {
        return ( size == 41724600 && crc == 775764504u ) ||
               ( size == 41726328 && crc == 131044578u );
    }

    inline uint32_t Crc32( std::istream& input )
    {
        static const auto table = []
        {
            std::array<uint32_t, 256> out{};
            for ( unsigned i = 0; i < 256; ++i )
            {
                uint32_t crc = i;
                for ( int bit = 0; bit < 8; ++bit )
                    crc = ( crc >> 1 ) ^ ( 0xedb88320u & ( 0u - ( crc & 1 ) ) );
                out[i] = crc;
            }
            return out;
        }();
        uint32_t crc = UINT32_MAX;
        char buffer[8192];
        while ( input.read( buffer, sizeof( buffer ) ) || input.gcount() )
            for ( std::streamsize i = 0; i < input.gcount(); ++i )
                crc = table[( crc ^ uint8_t( buffer[i] ) ) & 255] ^ ( crc >> 8 );
        return ~crc;
    }

    inline std::string LinuxMappedPath( std::istream& maps, uintptr_t address )
    {
        std::string line;

        while ( std::getline( maps, line ) )
        {
            std::istringstream input( line );
            std::string span, permissions, offset, device, inode, path;
            if ( !( input >> span >> permissions >> offset >> device >> inode ) )
                continue;

            auto dash = span.find( '-' );

            if ( dash == std::string::npos )
                continue;

            try
            {
                auto first = std::stoull( span.substr( 0, dash ), nullptr, 16 ), last = std::stoull( span.substr( dash + 1 ), nullptr, 16 );
                if ( address < first || address >= last )
                    continue;
            }
            catch ( ... )
            {
                continue;
            }

            std::getline( input >> std::ws, path );

            if ( path.empty() || path[0] != '/' || ( path.size() >= 10 && path.compare( path.size() - 10, 10, " (deleted)" ) == 0 ) )
                return {};

            std::string decoded;

            for ( size_t i = 0; i < path.size(); ++i )
            {
                if ( path[i] == '\\' && i + 3 < path.size() && path[i + 1] >= '0' && path[i + 1] <= '7' && path[i + 2] >= '0' && path[i + 2] <= '7' &&
                    path[i + 3] >= '0' && path[i + 3] <= '7' )
                {
                    decoded += char( ( path[i + 1] - '0' ) * 64 + ( path[i + 2] - '0' ) * 8 + ( path[i + 3] - '0' ) );
                    i += 3;
                }
                else
                    decoded += path[i];
            }

            return decoded;
        }

        return {};
    }

    inline bool Blocked( const std::vector<const Volume*>& volumes, Point start, Point end, float prediction, int& cellBudget )
    {
        if ( !Finite( start ) || !Finite( end ) || !std::isfinite( prediction ) )
            return false;

        float total = 0;

        for ( const auto* volume : volumes )
        {
            float entry, exit;

            if ( !volume || !Clip( volume->center, start, end - start, entry, exit ) )
                continue;

            if ( cellBudget < 128 )
                return false;

            cellBudget -= 128;
            total += Contribution( *volume, start, end, prediction );

            if ( total >= .2f )
                return true;
        }

        return false;
    }

    template<class Endpoints> bool SamplesBlocked( const std::vector<const Volume*>& volumes, int count, int first,
                                                  float prediction, int& cellBudget, Endpoints endpoints )
    {
        if ( volumes.empty() || ( count != 12 && count != 24 ) || first < 0 || first >= count ||
             !std::isfinite( prediction ) || prediction < 0 ) return false;
        for ( int order = 0; order < count; ++order )
        {
            int index = awh::OrderedSample( order, first );
            auto ray = endpoints( index );
            if ( !Blocked( volumes, ray.first, ray.second, index >= 12 ? prediction : 0, cellBudget ) ) return false;
        }
        return true;
    }
}
