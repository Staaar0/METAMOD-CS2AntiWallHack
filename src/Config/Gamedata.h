#pragma once
#include "Smoke/SmokeLayout.h"
#include <json.hpp>
#include <iomanip>
#include <sstream>
#include <set>
#include <stdexcept>

namespace awh
{
    inline nlohmann::json ParseGamedata( std::istream& input )
    {
        nlohmann::json platform;
        platform["ServerBinaries"] = nlohmann::json::array();
        const std::set<std::string> numbers{ "GameEntitySystem", "FireEvent",   "SmokeVolume",  "SmokeStorage",    "SmokeFrame",        "SmokeCenter",
                                             "SmokeStartTime",   "SmokeOpaque", "SmokeDensity", "SmokeFrameBytes", "SmokeDensityStride" };
        std::set<std::string> seen;
        std::string line;
        int lineNumber = 0;

        while ( std::getline( input, line ) )
        {
            ++lineNumber;
            std::istringstream tokens( line );
            tokens >> std::ws;

            if ( tokens.eof() || tokens.peek() == '#' || tokens.peek() == '/' )
            {
                continue;
            }

            std::string key, value, extra;

            if ( !( tokens >> key >> std::quoted( value ) ) )
            {
                throw std::runtime_error( "Missing gamedata value at line " + std::to_string( lineNumber ) );
            }

            if ( key == "ServerBinary" )
            {
                std::string crc;

                if ( !( tokens >> crc ) )
                {
                    throw std::runtime_error( "ServerBinary needs size and CRC32" );
                }

                auto parse = []( const std::string& text, uint64_t maximum )
                {
                    size_t used = 0;
                    auto result = std::stoull( text, &used, 10 );

                    if ( text.empty() || text[0] == '-' || used != text.size() || result == 0 || result > maximum )
                    {
                        throw std::runtime_error( "Invalid server fingerprint" );
                    }

                    return result;
                };

                platform["ServerBinaries"].push_back( { { "size", parse( value, INT64_MAX ) }, { "crc", parse( crc, UINT32_MAX ) } } );
            }
            else
            {
                if ( !seen.insert( key ).second )
                {
                    throw std::runtime_error( "Duplicate gamedata key: " + key );
                }

                if ( numbers.count( key ) )
                {
                    size_t used = 0;
                    auto parsed = std::stoll( value, &used, 10 );

                    if ( used != value.size() || parsed < 0 || parsed > INT32_MAX )
                    {
                        throw std::runtime_error( "Invalid gamedata number: " + key );
                    }

                    platform[key] = parsed;
                }
                else if ( key == "TraceShape" || key == "GameEventManager" )
                {
                    platform[key] = value;
                }
                else
                {
                    throw std::runtime_error( "Unknown gamedata key: " + key );
                }
            }

            if ( tokens >> extra && extra.rfind( "//", 0 ) != 0 && extra.rfind( "#", 0 ) != 0 )
            {
                throw std::runtime_error( "Unexpected gamedata text at line " + std::to_string( lineNumber ) );
            }
        }

        for ( const auto& key : numbers )
        {
            if ( !platform.contains( key ) )
            {
                throw std::runtime_error( "Missing gamedata key: " + key );
            }
        }

        if ( !platform.contains( "TraceShape" ) || !platform.contains( "GameEventManager" ) || platform["ServerBinaries"].empty() )
        {
            throw std::runtime_error( "Incomplete gamedata signatures or fingerprints" );
        }

        return { { "linux", platform } };
    }

    inline smoke::Layout SmokeLayout( const nlohmann::json& platform )
    {
        smoke::Layout layout;
        layout.volume = platform.at( "SmokeVolume" );
        layout.storage = platform.at( "SmokeStorage" );
        layout.frame = platform.at( "SmokeFrame" );
        layout.center = platform.at( "SmokeCenter" );
        layout.startTime = platform.at( "SmokeStartTime" );
        layout.opaque = platform.at( "SmokeOpaque" );
        layout.density = platform.at( "SmokeDensity" );
        layout.frameBytes = platform.at( "SmokeFrameBytes" );
        layout.densityStride = platform.at( "SmokeDensityStride" );

        if ( !layout.Valid() )
        {
            throw std::runtime_error( "Invalid smoke layout in gamedata.txt" );
        }

        return layout;
    }
}
