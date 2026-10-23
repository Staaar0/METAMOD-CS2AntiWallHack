#pragma once
#include <json.hpp>
#include <array>
#include <optional>
#include <string>
#include <sstream>
#include <stdexcept>

namespace awh::updates
{
    inline constexpr const char* Repository = "ZusDev/METAMOD-CS2AntiWallHack";
    inline constexpr const char* AssetName = "AntiWallHack-Metamod-linux.zip";
    inline constexpr const char* BinaryEntry = "addons/antiwallhack/bin/linuxsteamrt64/antiwallhack.so";
    inline constexpr const char* GamedataEntry = "addons/antiwallhack/gamedata.txt";
    inline constexpr const char* MapsAssetName = "maps.zip";
    inline constexpr const char* VdfEntry = "addons/metamod/antiwallhack.vdf";
    inline constexpr const char* ConfigEntry = "cfg/AntiWallHack.cfg";
    inline constexpr size_t MapsArchiveLimit = 256 * 1024 * 1024;
    inline constexpr size_t ArchiveLimit = 64 * 1024 * 1024;

    inline std::optional<std::array<int, 3>> Version( std::string text )
    {
        if ( !text.empty() && text.front() == 'v' )
        {
            text.erase( 0, 1 );
        }

        std::array<int, 3> result{};
        std::istringstream input( text );

        for ( int index = 0; index < 3; ++index )
        {
            if ( input.peek() < '0' || input.peek() > '9' || !( input >> result[index] ) || result[index] < 0 )
            {
                return std::nullopt;
            }

            if ( index < 2 && input.get() != '.' )
            {
                return std::nullopt;
            }
        }

        if ( input.peek() != std::char_traits<char>::eof() )
        {
            return std::nullopt;
        }

        return result;
    }

    struct Asset
    {
        std::string url;
        std::string digest;
        size_t size = 0;
    };

    struct Release
    {
        std::string version;
        Asset plugin;
        Asset maps;
    };

    inline Asset SelectAsset( const nlohmann::json& assets, const std::string& tag, const std::string& name, size_t limit )
    {
        std::optional<Asset> selected;

        for ( const auto& asset : assets )
        {
            if ( asset.at( "name" ) != name )
            {
                continue;
            }

            Asset result{ asset.at( "browser_download_url" ).get<std::string>(), asset.value( "digest", "" ), asset.at( "size" ).get<size_t>() };
            const std::string expected = "https://github.com/" + std::string( Repository ) + "/releases/download/" + tag + "/" + name;

            if ( selected || result.url != expected || result.size == 0 || result.size > limit || result.digest.size() != 71 || result.digest.rfind( "sha256:", 0 ) != 0 ||
                 result.digest.find_first_not_of( "0123456789abcdef", 7 ) != std::string::npos )
            {
                throw std::runtime_error( "Release asset URL, size or SHA256 digest is invalid" );
            }

            selected = result;
        }

        if ( !selected )
        {
            throw std::runtime_error( "New release must include both the Linux plugin ZIP and maps.zip" );
        }

        return *selected;
    }

    inline std::optional<Release> SelectRelease( const nlohmann::json& data, const std::string& current )
    {
        if ( data.value( "draft", false ) || data.value( "prerelease", false ) )
        {
            return std::nullopt;
        }

        const std::string tag = data.at( "tag_name" ).get<std::string>();
        auto candidate = Version( tag );
        auto installed = Version( current );

        if ( !candidate || !installed || *candidate <= *installed )
        {
            return std::nullopt;
        }

        return Release{ tag, SelectAsset( data.at( "assets" ), tag, AssetName, ArchiveLimit ), SelectAsset( data.at( "assets" ), tag, MapsAssetName, MapsArchiveLimit ) };
    }
}
