#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace awh::smoke
{
    struct Fingerprint
    {
        uint64_t size;
        uint32_t crc;
    };

    struct Layout
    {
        size_t volume = 3736;
        size_t storage = 112;
        size_t frame = 236;
        size_t center = 212;
        size_t startTime = 12;
        size_t opaque = 8;
        size_t density = 0x3008;
        size_t frameBytes = 0x80000;
        size_t densityStride = 16;

        bool Valid() const
        {
            return volume >= 256 && volume <= 16384 && storage <= 504 && frame <= 508 && center <= 500 && startTime <= 508 && opaque <= 65536 && density <= 65536 &&
                   densityStride >= sizeof( float ) && densityStride <= 64 && frameBytes >= 32768 * densityStride && frameBytes <= 0x200000;
        }
    };

    inline bool Matches( uint64_t size, uint32_t crc, const std::vector<Fingerprint>& accepted )
    {
        for ( const auto& fingerprint : accepted )
        {
            if ( fingerprint.size == size && fingerprint.crc == crc )
            {
                return true;
            }
        }

        return false;
    }
}
