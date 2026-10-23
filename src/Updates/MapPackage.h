#pragma once
#include "Visibility/MapIdentity.h"
#include <json.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace awh::updates
{
    inline constexpr size_t MapFileLimit = 256 * 1024 * 1024 + 65540;
    inline constexpr size_t MapsTotalLimit = 1024 * 1024 * 1024;

    inline std::optional<std::string> MapEntry( const std::string& entry )
    {
        if ( entry.empty() || entry.back() == '/' )
        {
            return std::nullopt;
        }

        std::string relative;

        for ( const char* prefix : { "maps/addons/antiwallhack/vis/", "addons/antiwallhack/vis/" } )
        {
            const std::string root( prefix );

            if ( entry.rfind( root, 0 ) == 0 )
            {
                relative = entry.substr( root.size() );
                break;
            }
        }

        if ( relative.size() <= 6 || relative.substr( relative.size() - 6 ) != ".awvis" )
        {
            throw std::runtime_error( "maps.zip contains an unexpected file" );
        }

        const std::string key = relative.substr( 0, relative.size() - 6 );

        if ( MapIdentity( key ) != key )
        {
            throw std::runtime_error( "maps.zip contains an invalid map path" );
        }

        return relative;
    }

    inline void ValidateMapPackageFile( const std::filesystem::path& path, const std::string& relative, const std::function<bool()>& cancelled = {} )
    {
        std::ifstream input( path, std::ios::binary );
        uint32_t headerSize = 0;
        input.read( reinterpret_cast<char*>( &headerSize ), sizeof( headerSize ) );

        if ( !input || headerSize == 0 || headerSize > 65536 )
        {
            throw std::runtime_error( "Invalid map update manifest" );
        }

        std::string header( headerSize, '\0' );
        input.read( header.data(), headerSize );

        if ( !input )
        {
            throw std::runtime_error( "Truncated map update manifest" );
        }

        const auto data = nlohmann::json::parse( header );
        const std::string map = relative.substr( 0, relative.size() - 6 );

        if ( data.at( "version" ) != 1 || data.at( "cellSize" ) != 64 || data.at( "map" ) != map )
        {
            throw std::runtime_error( "Map update identity or format mismatch" );
        }

        const int nx = data.at( "nx" ), ny = data.at( "ny" ), nz = data.at( "nz" ), count = data.at( "count" );

        if ( nx <= 0 || nx > 4096 || ny <= 0 || ny > 4096 || nz <= 0 || nz > 4096 || count <= 0 || count > 32768 )
        {
            throw std::runtime_error( "Invalid map update dimensions" );
        }

        const uint64_t bytes = uint64_t( nx ) * ny * nz * 4 + uint64_t( count ) * 12 + uint64_t( count ) * ( ( count + 63 ) / 64 ) * 8;

        if ( bytes > 256ULL * 1024 * 1024 || std::filesystem::file_size( path ) != bytes + headerSize + 4 )
        {
            throw std::runtime_error( "Map update payload size mismatch" );
        }

        uint64_t hash = 14695981039346656037ULL;
        std::array<char, 65536> buffer{};

        while ( input )
        {
            if ( cancelled && cancelled() )
            {
                throw std::runtime_error( "Map update cancelled" );
            }

            input.read( buffer.data(), buffer.size() );

            for ( std::streamsize index = 0; index < input.gcount(); ++index )
            {
                hash = ( hash ^ static_cast<unsigned char>( buffer[index] ) ) * 1099511628211ULL;
            }

            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }

        if ( input.bad() || hash != data.at( "checksum" ).get<uint64_t>() )
        {
            throw std::runtime_error( "Map update checksum mismatch" );
        }
    }
}
