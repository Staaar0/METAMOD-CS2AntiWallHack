#pragma once
#include <json.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <chrono>
#include "Visibility/MapIdentity.h"

namespace awh
{
    // Native .awvis format is deliberately independent of engine/SDK layouts.
    class VisibilityMap
    {
        int nx = 0, ny = 0, nz = 0, count = 0, words = 0;
        std::array<float, 3> minimum{};
        std::vector<int32_t> cells;
        std::vector<std::array<float, 3>> floors;
        std::vector<uint64_t> bits;

        static uint64_t Hash( const char* bytes, size_t size, uint64_t hash = 14695981039346656037ULL )
        {
            for ( size_t i = 0; i < size; ++i )
            {
                hash = ( hash ^ static_cast<unsigned char>( bytes[i] ) ) * 1099511628211ULL;
            }

            return hash;
        }

        static uint64_t FileHash( const std::filesystem::path& path, uint64_t expectedSize, const std::function<bool()>& cancelled )
        {
            if ( std::filesystem::file_size( path ) != expectedSize )
            {
                throw std::runtime_error( "VIS source size changed: " + path.string() );
            }

            std::ifstream file( path, std::ios::binary );

            if ( !file )
            {
                throw std::runtime_error( "VIS source unavailable" );
            }

            std::array<char, 65536> buffer;
            uint64_t hash = 14695981039346656037ULL, size = 0;

            while ( file )
            {
                if ( cancelled && cancelled() )
                {
                    throw std::runtime_error( "VIS validation cancelled" );
                }

                file.read( buffer.data(), buffer.size() );
                size += file.gcount();
                hash = Hash( buffer.data(), size_t( file.gcount() ), hash );

                if ( size < expectedSize )
                {
                    std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                }
            }

            if ( file.bad() || size != expectedSize )
            {
                throw std::runtime_error( "Cannot read VIS source" );
            }

            return hash;
        }

      public:
        bool Ready() const
        {
            return count > 0;
        }

        size_t CellCount() const
        {
            return size_t( count );
        }

        void Load( const std::filesystem::path& path, const std::filesystem::path& root, const std::string& map, const std::function<void( const char* )>& progress = {},
                   const std::function<bool()>& cancelled = {} )
        {
            auto stage = [&]( const char* message )
            {
                if ( cancelled && cancelled() )
                {
                    throw std::runtime_error( "VIS validation cancelled" );
                }

                if ( progress )
                {
                    progress( message );
                }
            };
            *this = {};
            stage( "VIS load: resolving map archive" );
            VisibilityMap loaded;
            const bool workshop = map.compare( 0, 9, "workshop/" ) == 0;
            const auto archive = MapArchive( root, map );
            const auto sourceRoot = workshop ? archive.parent_path() : root;

            const auto expectedSource = archive.lexically_relative( sourceRoot ).generic_string();
            const auto sourceDescription = "VIS load: expected source=" + expectedSource;
            stage( sourceDescription.c_str() );
            std::ifstream file( path, std::ios::binary );
            stage( "VIS load: reading manifest" );

            if ( !file )
            {
                throw std::runtime_error( "VIS table missing" );
            }

            uint32_t headerSize = 0;
            file.read( reinterpret_cast<char*>( &headerSize ), 4 );

            if ( !file || headerSize == 0 || headerSize > 65536 )
            {
                throw std::runtime_error( "Invalid VIS manifest" );
            }

            std::string header( headerSize, '\0' );
            file.read( header.data(), headerSize );

            if ( !file )
            {
                throw std::runtime_error( "Truncated VIS manifest" );
            }

            stage( "VIS load: parsing manifest" );

            auto data = nlohmann::json::parse( header );

            if ( data.at( "version" ) != 1 || data.at( "map" ) != map || data.at( "cellSize" ) != 64 )
            {
                throw std::runtime_error( "VIS map/format mismatch" );
            }

            loaded.minimum = data.at( "min" ).get<std::array<float, 3>>();
            loaded.nx = data.at( "nx" ).get<int>();
            loaded.ny = data.at( "ny" ).get<int>();
            loaded.nz = data.at( "nz" ).get<int>();
            loaded.count = data.at( "count" ).get<int>();

            for ( float value : loaded.minimum )
            {
                if ( !std::isfinite( value ) )
                {
                    throw std::runtime_error( "Invalid VIS origin" );
                }
            }

            if ( loaded.nx <= 0 || loaded.nx > 4096 || loaded.ny <= 0 || loaded.ny > 4096 || loaded.nz <= 0 || loaded.nz > 4096 || loaded.count <= 0 || loaded.count > 32768 )
            {
                throw std::runtime_error( "Invalid VIS dimensions" );
            }

            uint64_t grid = uint64_t( loaded.nx ) * loaded.ny * loaded.nz;
            loaded.words = ( loaded.count + 63 ) / 64;
            uint64_t bitCount = uint64_t( loaded.count ) * loaded.words;
            uint64_t bytes = grid * 4 + uint64_t( loaded.count ) * 12 + bitCount * 8;

            if ( bytes > 256ULL * 1024 * 1024 || std::filesystem::file_size( path ) != bytes + headerSize + 4 )
            {
                throw std::runtime_error( "VIS table too large or truncated" );
            }

            auto sources = data.at( "sources" );
            stage( "VIS load: dimensions checked, verifying archive fingerprints" );

            if ( !sources.is_array() || sources.empty() || sources.size() > 128 )
            {
                throw std::runtime_error( "Invalid VIS sources" );
            }

            bool mapSource = false;

            for ( const auto& source : sources )
            {
                const std::string name = source.at( "path" ).get<std::string>();
                std::filesystem::path relative( name );

                if ( name.empty() || name.find( '\\' ) != std::string::npos || name.find( ':' ) != std::string::npos || relative.is_absolute() )
                {
                    throw std::runtime_error( "Invalid VIS source path" );
                }

                for ( const auto& part : relative )
                {
                    if ( part == ".." || part == "." )
                    {
                        throw std::runtime_error( "Invalid VIS source path" );
                    }
                }

                if ( workshop && name.find( '/' ) != std::string::npos )
                {
                    throw std::runtime_error( "Invalid Workshop source path" );
                }

                if ( name == expectedSource )
                {
                    mapSource = true;
                }

                const auto sourceSize = source.at( "size" ).get<uint64_t>();
                const auto started = std::chrono::steady_clock::now();
                const std::string description = "VIS source hash: " + name + " bytes=" + std::to_string( sourceSize );
                stage( description.c_str() );

                if ( FileHash( sourceRoot / relative, sourceSize, cancelled ) != source.at( "hash" ).get<uint64_t>() )
                {
                    throw std::runtime_error( "VIS source changed, rebake the map" );
                }

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started ).count();
                const std::string completed = "VIS source hash complete: " + name + " elapsed_ms=" + std::to_string( elapsed );
                stage( completed.c_str() );
            }

            stage( mapSource ? "VIS load: source loop completed, current archive matched" : "VIS load: source loop completed, current archive missing" );

            if ( !mapSource )
            {
                throw std::runtime_error( "VIS manifest lacks current map VPK: " + expectedSource );
            }

            stage( "VIS load: fingerprints verified, allocating table" );

            if ( progress )
            {
                const std::string allocation = "VIS load: allocation bytes=" + std::to_string( bytes ) + " cells=" + std::to_string( loaded.count );
                stage( allocation.c_str() );
            }

            loaded.cells.resize( size_t( grid ) );
            loaded.floors.resize( loaded.count );
            loaded.bits.resize( size_t( bitCount ) );
            stage( "VIS load: allocation complete, reading payload" );
            uint64_t hash = 14695981039346656037ULL;

            auto read = [&]( void* buffer, size_t size )
            {
                auto* bytes = static_cast<char*>( buffer );

                for ( size_t offset = 0; offset < size; )
                {
                    if ( cancelled && cancelled() )
                    {
                        throw std::runtime_error( "VIS validation cancelled" );
                    }

                    size_t chunk = std::min( size - offset, size_t( 65536 ) );
                    file.read( bytes + offset, chunk );

                    if ( !file )
                    {
                        throw std::runtime_error( "Truncated VIS payload" );
                    }

                    hash = Hash( bytes + offset, chunk, hash );
                    offset += chunk;

                    if ( offset < size )
                    {
                        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                    }
                }
            };

            read( loaded.cells.data(), loaded.cells.size() * 4 );
            read( loaded.floors.data(), loaded.floors.size() * 12 );
            read( loaded.bits.data(), loaded.bits.size() * 8 );
            stage( "VIS load: payload read, validating checksum and cells" );

            if ( hash != data.at( "checksum" ).get<uint64_t>() )
            {
                throw std::runtime_error( "VIS checksum mismatch" );
            }

            for ( int cell : loaded.cells )
            {
                if ( cell < -1 || cell >= loaded.count )
                {
                    throw std::runtime_error( "Invalid VIS cell index" );
                }
            }

            for ( const auto& floor : loaded.floors )
            {
                for ( float value : floor )
                {
                    if ( !std::isfinite( value ) )
                    {
                        throw std::runtime_error( "Invalid VIS floor" );
                    }
                }
            }

            stage( "VIS load: table validated" );
            *this = std::move( loaded );
        }

        template <class Point> int Cell( const Point& origin ) const
        {
            const float position[] = { origin.x, origin.y, origin.z };
            int index[3];
            const int limit[] = { nx, ny, nz };

            for ( int i = 0; i < 3; ++i )
            {
                double value = std::floor( ( double( position[i] ) - minimum[i] ) / 64 );

                if ( !std::isfinite( value ) || value < 0 || value >= limit[i] )
                {
                    return -1;
                }

                index[i] = int( value );
            }

            for ( int z = index[2]; z >= index[2] - 1 && z >= 0; --z )
            {
                int cell = cells[( size_t( z ) * ny + index[1] ) * nx + index[0]];
                // Airborne, ramp and stacked-floor uncertainty uses real engine rays.
                if ( cell >= 0 && std::abs( origin.z - floors[cell][2] ) <= 16 )
                {
                    return cell;
                }
            }

            return -1;
        }

        bool Visible( int from, int to ) const
        {
            if ( from < 0 || to < 0 || from >= count || to >= count )
            {
                return true;
            }

            if ( from == to )
            {
                return true;
            }

            return ( bits[size_t( from ) * words + ( to >> 6 )] & ( uint64_t( 1 ) << ( to & 63 ) ) ) != 0;
        }

        template <class Point> bool Blocked( const Point& viewer, const Point& viewerAhead, const Point& target, const Point& targetAhead ) const
        {
            if ( !Ready() )
            {
                return false;
            }

            int a = Cell( viewer ), b = Cell( viewerAhead ), c = Cell( target ), d = Cell( targetAhead );
            return BlockedCells( a, b, c, d );
        }

        bool BlockedCells( int a, int b, int c, int d ) const
        {
            if ( !Ready() )
            {
                return false;
            }

            // Symmetric check makes disagreement conservative. Never turn an unknown cell into a blocker.
            return !Visible( a, c ) && !Visible( a, d ) && !Visible( b, c ) && !Visible( b, d ) && !Visible( c, a ) && !Visible( d, a ) && !Visible( c, b ) && !Visible( d, b );
        }
    };
}
