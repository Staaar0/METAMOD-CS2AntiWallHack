#pragma once
#include <filesystem>
#include <string>
#include <set>
#include <vector>
#include <stdexcept>

namespace awh
{
    inline bool MapToken( const std::string& value )
    {
        return !value.empty() && value.size() <= 128 &&
               value.find_first_not_of( "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" ) == std::string::npos;
    }

    inline std::string MapIdentity( std::string map, std::string addon = {} )
    {
        if ( map.compare( 0, 9, "workshop/" ) == 0 )
        {
            auto split = map.find( '/', 9 );
            if ( split == std::string::npos )
                return {};
            auto id = map.substr( 9, split - 9 );
            if ( !addon.empty() && addon != id )
                return {};
            addon = id;
            map = map.substr( split + 1 );
        }
        if ( !MapToken( map ) || ( !addon.empty() && !MapToken( addon ) ) )
            return {};
        return addon.empty() ? map : "workshop/" + addon + "/" + map;
    }

    inline bool ArchiveChunk( const std::filesystem::path& path )
    {
        auto name = path.stem().string();
        auto split = name.rfind( '_' );
        return split != std::string::npos && name.size() - split == 4 && name.find_first_not_of( "0123456789", split + 1 ) == std::string::npos;
    }

    inline std::filesystem::path MapArchive( const std::filesystem::path& root, const std::string& key )
    {
        namespace fs = std::filesystem;
        if ( key == MapIdentity( key ) && MapToken( key ) )
            return root / "maps" / ( key + ".vpk" );
        if ( key.empty() || key != MapIdentity( key ) )
            throw std::runtime_error( "Invalid map identity" );
        auto split = key.find( '/', 9 );
        auto addon = key.substr( 9, split - 9 ), map = key.substr( split + 1 );
        std::vector<fs::path> folders{ root / "maps/workshop" / addon };
        auto current = fs::absolute( root );
        for ( int depth = 0; depth < 5 && !current.empty(); ++depth )
        {
            folders.push_back( current / "csgo_addons" / addon );
            folders.push_back( current / "csgo_community_addons" / addon );
            folders.push_back( current / "steamapps/workshop/content/730" / addon );
            if ( current.filename() == "steamapps" )
                folders.push_back( current / "workshop/content/730" / addon );
            if ( fs::is_directory( current / "bin" ) )
                for ( const auto& platform : fs::directory_iterator( current / "bin" ) )
                    if ( platform.is_directory() )
                        folders.push_back( platform.path() / "steamapps/workshop/content/730" / addon );
            auto parent = current.parent_path();
            if ( parent == current )
                break;
            current = parent;
        }
        std::set<fs::path> candidates;
        for ( const auto& folder : folders )
            if ( fs::is_directory( folder ) )
                for ( const auto& entry : fs::recursive_directory_iterator( folder ) )
                    if ( entry.is_regular_file() && entry.path().extension() == ".vpk" && !ArchiveChunk( entry.path() ) )
                        candidates.insert( fs::absolute( entry.path() ).lexically_normal() );
        // Multiple containers could supply the same map. Do not guess mount precedence.
        if ( candidates.size() == 1 )
            return *candidates.begin();
        throw std::runtime_error( "Workshop archive missing or ambiguous for " + key );
    }
}
