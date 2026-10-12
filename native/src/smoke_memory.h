#pragma once
#include "smoke.h"
#include <fstream>
#include <filesystem>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace awh::smoke
{
    inline bool ReadMemory( uintptr_t address, void* output, size_t size )
    {
        if ( !address || !output || !size || size > uintptr_t( INTPTR_MAX ) || address > uintptr_t( INTPTR_MAX ) - size )
            return false;
        iovec local{ output, size }, remote{ reinterpret_cast<void*>( address ), size };
        return syscall( SYS_process_vm_readv, getpid(), &local, 1ul, &remote, 1ul, 0ul ) == static_cast<long>( size );
    }

    template <class T> bool ReadValue( uintptr_t address, T& output )
    {
        return ReadMemory( address, &output, sizeof( output ) );
    }

    inline bool VerifyModule( void* anchor, std::string& reason )
    {
        try
        {
            std::filesystem::path path;
            std::ifstream maps( "/proc/self/maps" );
            path = LinuxMappedPath( maps, reinterpret_cast<uintptr_t>( anchor ) );
            constexpr uint64_t expectedSize = 41724600;
            if ( !anchor || path.empty() )
            {
                reason = "server module mapping missing or deleted";
                return false;
            }

            std::ifstream file( path, std::ios::binary | std::ios::ate );

            if ( !file )
            {
                reason = "cannot read mapped server binary";
                return false;
            }

            auto size = file.tellg();

            if ( size < 0 || static_cast<uint64_t>( size ) != expectedSize )
            {
                reason = "unsupported server binary size: " + std::to_string( static_cast<long long>( size ) );
                return false;
            }

            file.seekg( 0 );
            auto crc = Crc32( file );

            if ( file.bad() || !Matches( static_cast<uint64_t>( size ), crc ) )
            {
                reason = "unsupported server CRC32: " + std::to_string( crc );
                return false;
            }

            return true;
        }
        catch ( const std::exception& e )
        {
            reason = e.what();
            return false;
        }
    }
}
