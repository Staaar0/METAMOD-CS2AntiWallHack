#pragma once
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>
#include <fstream>
#include <link.h>
#include <dlfcn.h>
#include <cstring>

namespace awh
{
    inline bool Readable( const void* pointer, size_t size )
    {
        auto address = reinterpret_cast<uintptr_t>( pointer );

        if ( !address || size > UINTPTR_MAX - address )
            return false;

        static const auto ranges = []
        {
            std::vector<std::pair<uintptr_t, uintptr_t>> result;
            std::ifstream maps( "/proc/self/maps" );
            std::string line;
            while ( std::getline( maps, line ) )
            {
                std::istringstream input( line );
                std::string span, permissions;
                input >> span >> permissions;

                auto dash = span.find( '-' );
                if ( dash != std::string::npos && !permissions.empty() && permissions[0] == 'r' )
                    result.emplace_back( std::strtoull( span.substr( 0, dash ).c_str(), nullptr, 16 ),
                                         std::strtoull( span.substr( dash + 1 ).c_str(), nullptr, 16 ) );
            }
            return result;
        }();

        for ( auto range : ranges )
            if ( address >= range.first && address + size <= range.second )
                return true;

        return false;
    }

    inline bool SchemaNameEquals( const char* name, const char* expected )
    {
        const size_t length = std::strlen( expected ) + 1;
        return Readable( name, length ) && !std::memcmp( name, expected, length );
    }

    struct Region
    {
        const uint8_t* start;
        size_t size;
    };

    inline void* FindServerFunction( const std::string& pattern, void* serverFunction )
    {
        if ( !serverFunction )
            return nullptr;

        std::vector<int> bytes;
        std::istringstream input( pattern );
        std::string token;

        while ( input >> token )
        {
            if ( token == "?" || token == "??" )
                bytes.push_back( -1 );
            else
            {
                if ( token.size() != 2 || token.find_first_not_of( "0123456789abcdefABCDEF" ) != std::string::npos )
                    return nullptr;

                bytes.push_back( static_cast<int>( std::strtoul( token.c_str(), nullptr, 16 ) ) );
            }
        }

        if ( bytes.empty() )
            return nullptr;

        std::vector<Region> regions;

        Dl_info module{};

        if ( !dladdr( serverFunction, &module ) || !module.dli_fbase )
            return nullptr;

        struct Search
        {
            void* base;
            std::vector<Region>* regions;
        } search{ module.dli_fbase, &regions };

        dl_iterate_phdr(
            []( dl_phdr_info* info, size_t, void* data )
            {
                auto& search = *static_cast<Search*>( data );

                if ( reinterpret_cast<void*>( info->dlpi_addr ) != search.base )
                    return 0;

                auto& out = *search.regions;

                for ( unsigned i = 0; i < info->dlpi_phnum; ++i )
                {
                    auto& p = info->dlpi_phdr[i];

                    if ( p.p_type == PT_LOAD && ( p.p_flags & PF_X ) )
                        out.push_back( { reinterpret_cast<const uint8_t*>( info->dlpi_addr + p.p_vaddr ), size_t( p.p_memsz ) } );
                }
                return 1;
            },
            &search );
        const uint8_t* match = nullptr;

        for ( auto region : regions )
        {
            for ( size_t i = 0; i + bytes.size() <= region.size; ++i )
            {
                size_t j = 0;

                while ( j < bytes.size() && ( bytes[j] < 0 || region.start[i + j] == bytes[j] ) )
                    ++j;

                if ( j == bytes.size() )
                {
                    if ( match )
                        return nullptr;

                    match = region.start + i;
                }
            }
        }
        return const_cast<uint8_t*>( match );
    }
}
