#include "Updates/ReleaseUpdater.h"
#include "Updates/ReleasePolicy.h"
#include "Updates/MapPackage.h"
#include "Config/Config.h"
#include <set>
#include "Config/Gamedata.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>
#include <vector>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

extern char** environ;

namespace awh::updates
{
    namespace fs = std::filesystem;

    class UpdateLock
    {
        int descriptor = -1;

      public:
        explicit UpdateLock( const fs::path& path )
        {
            descriptor = open( path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600 );

            if ( descriptor < 0 || flock( descriptor, LOCK_EX | LOCK_NB ) != 0 )
            {
                if ( descriptor >= 0 )
                {
                    close( descriptor );
                    descriptor = -1;
                }
            }
        }

        ~UpdateLock()
        {
            if ( descriptor >= 0 )
            {
                close( descriptor );
            }
        }

        bool Acquired() const
        {
            return descriptor >= 0;
        }
    };

    class StagingDirectory
    {
      public:
        fs::path path;

        explicit StagingDirectory( const fs::path& parent )
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = parent / ( "staging-" + std::to_string( getpid() ) + "-" + std::to_string( stamp ) );

            if ( !fs::create_directory( path ) )
            {
                throw std::runtime_error( "Cannot create update staging directory" );
            }
        }

        ~StagingDirectory()
        {
            std::error_code error;
            fs::remove_all( path, error );
        }
    };

    static void Run( const std::vector<std::string>& arguments, const fs::path& output, size_t limit, const std::shared_ptr<std::atomic<bool>>& cancel )
    {
        if ( cancel->load() )
        {
            throw std::runtime_error( "Update cancelled" );
        }

        std::vector<char*> argv;

        for ( const auto& value : arguments )
        {
            argv.push_back( const_cast<char*>( value.c_str() ) );
        }

        argv.push_back( nullptr );
        posix_spawn_file_actions_t actions;

        if ( posix_spawn_file_actions_init( &actions ) != 0 )
        {
            throw std::runtime_error( "Cannot initialize update process" );
        }

        int error = posix_spawn_file_actions_addopen( &actions, STDOUT_FILENO, output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600 );

        if ( !error )
        {
            error = posix_spawn_file_actions_addopen( &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0 );
        }

        if ( !error )
        {
            error = posix_spawn_file_actions_addopen( &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0 );
        }

        pid_t child = -1;

        if ( !error )
        {
            error = posix_spawnp( &child, argv[0], &actions, nullptr, argv.data(), environ );
        }

        posix_spawn_file_actions_destroy( &actions );

        if ( error )
        {
            throw std::runtime_error( "Cannot start update helper, install curl, unzip and sha256sum" );
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 75 );
        int status = 0;

        while ( true )
        {
            pid_t result = waitpid( child, &status, WNOHANG );

            if ( result == child )
            {
                break;
            }

            if ( result < 0 && errno != EINTR )
            {
                throw std::runtime_error( "Cannot wait for update helper" );
            }

            std::error_code sizeError;
            auto size = fs::file_size( output, sizeError );

            if ( cancel->load() || std::chrono::steady_clock::now() >= deadline || ( !sizeError && size > limit ) )
            {
                kill( child, SIGKILL );

                while ( waitpid( child, &status, 0 ) < 0 && errno == EINTR )
                {
                }

                throw std::runtime_error( "Update helper cancelled or exceeded its time/size limit" );
            }

            std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
        }

        if ( !WIFEXITED( status ) || WEXITSTATUS( status ) != 0 || !fs::is_regular_file( output ) || fs::file_size( output ) > limit )
        {
            throw std::runtime_error( "Update download, checksum or extraction failed" );
        }
    }

    static std::string ReadText( const fs::path& path )
    {
        std::ifstream input( path, std::ios::binary );

        if ( !input )
        {
            throw std::runtime_error( "Cannot read update staging file" );
        }

        return std::string( std::istreambuf_iterator<char>( input ), {} );
    }

    static std::vector<std::string> Curl( const std::string& url, size_t limit )
    {
        return { "curl",
                 "--fail",
                 "--silent",
                 "--location",
                 "--proto",
                 "=https",
                 "--proto-redir",
                 "=https",
                 "--connect-timeout",
                 "10",
                 "--max-time",
                 "60",
                 "--max-filesize",
                 std::to_string( limit ),
                 "--user-agent",
                 "AntiWallHack-Updater",
                 "--header",
                 "Accept: application/vnd.github+json",
                 url };
    }

    static void ValidateBinary( const fs::path& path )
    {
        std::ifstream input( path, std::ios::binary );
        std::array<unsigned char, 64> header{};
        input.read( reinterpret_cast<char*>( header.data() ), header.size() );

        if ( !input || header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F' || header[4] != 2 || header[5] != 1 || header[16] != 3 || header[17] != 0 ||
             header[18] != 62 || header[19] != 0 )
        {
            throw std::runtime_error( "Release binary is not an x86-64 Linux shared object" );
        }
    }

    Result CheckAndInstall( const fs::path& gamePath, const std::string& current, const std::shared_ptr<std::atomic<bool>>& cancel )
    {
        try
        {
            const fs::path plugin = gamePath / "addons/antiwallhack";
            const fs::path updates = plugin / "updates";
            fs::create_directories( updates );
            UpdateLock lock( updates / "update.lock" );

            if ( !lock.Acquired() )
            {
                return { false, "Another update check is already running" };
            }

            StagingDirectory staging( updates );
            auto metadata = staging.path / "release.json";
            Run( Curl( "https://api.github.com/repos/" + std::string( Repository ) + "/releases/latest", 1024 * 1024 ), metadata, 1024 * 1024, cancel );
            auto release = SelectRelease( nlohmann::json::parse( ReadText( metadata ) ), current );

            if ( !release )
            {
                return { false, "" };
            }

            auto download = [&]( const Asset& asset, const fs::path& path, size_t limit )
            {
                Run( Curl( asset.url, limit ), path, limit, cancel );

                if ( fs::file_size( path ) != asset.size )
                {
                    throw std::runtime_error( "Release asset size does not match GitHub metadata" );
                }

                const auto checksum = staging.path / "checksum.txt";
                Run( { "sha256sum", path.string() }, checksum, 1024, cancel );

                if ( ReadText( checksum ).substr( 0, 64 ) != asset.digest.substr( 7 ) )
                {
                    throw std::runtime_error( "Release SHA256 verification failed" );
                }
            };

            const auto archive = staging.path / "release.zip";
            const auto mapsArchive = staging.path / "maps.zip";
            download( release->plugin, archive, ArchiveLimit );
            download( release->maps, mapsArchive, MapsArchiveLimit );

            const auto binary = staging.path / "antiwallhack.so";
            const auto gamedata = staging.path / "gamedata.txt";
            const auto vdf = staging.path / "antiwallhack.vdf";
            const auto config = staging.path / "AntiWallHack.cfg";
            Run( { "unzip", "-p", archive.string(), BinaryEntry }, binary, 32 * 1024 * 1024, cancel );
            Run( { "unzip", "-p", archive.string(), GamedataEntry }, gamedata, 1024 * 1024, cancel );
            Run( { "unzip", "-p", archive.string(), VdfEntry }, vdf, 65536, cancel );
            Run( { "unzip", "-p", archive.string(), ConfigEntry }, config, 65536, cancel );
            std::ifstream configInput( config );
            ParseConfig( configInput );
            ValidateBinary( binary );
            std::ifstream gamedataInput( gamedata );
            auto parsed = ParseGamedata( gamedataInput );
            SmokeLayout( parsed.at( "linux" ) );

            const fs::path targetBinary = plugin / "bin/linuxsteamrt64/antiwallhack.so";
            const fs::path targetGamedata = plugin / "gamedata.txt";
            std::vector<std::pair<fs::path, fs::path>> files{ { gamedata, targetGamedata }, { vdf, gamePath / VdfEntry } };

            if ( !fs::exists( gamePath / ConfigEntry ) )
            {
                files.emplace_back( config, gamePath / ConfigEntry );
            }

            const auto listing = staging.path / "map-entries.txt";
            Run( { "unzip", "-Z1", mapsArchive.string() }, listing, 1024 * 1024, cancel );
            std::istringstream entries( ReadText( listing ) );
            std::string entry;
            std::set<std::string> mapNames;
            uint64_t totalBytes = 0;

            while ( std::getline( entries, entry ) )
            {
                if ( !entry.empty() && entry.back() == '\r' )
                {
                    entry.pop_back();
                }

                auto relative = MapEntry( entry );

                if ( !relative )
                {
                    continue;
                }

                if ( !mapNames.insert( *relative ).second || mapNames.size() > 128 )
                {
                    throw std::runtime_error( "Duplicate or excessive maps.zip entries" );
                }

                const auto stagedMap = staging.path / ( "map-" + std::to_string( mapNames.size() ) + ".awvis" );
                Run( { "unzip", "-p", mapsArchive.string(), entry }, stagedMap, MapFileLimit, cancel );
                totalBytes += fs::file_size( stagedMap );

                if ( totalBytes > MapsTotalLimit )
                {
                    throw std::runtime_error( "Map update exceeds its total size limit" );
                }

                ValidateMapPackageFile( stagedMap, *relative,
                                        [&]()
                                        {
                                            return cancel->load();
                                        } );
                files.emplace_back( stagedMap, plugin / "vis" / *relative );
            }

            if ( mapNames.empty() )
            {
                throw std::runtime_error( "maps.zip contains no baked VIS maps" );
            }

            files.emplace_back( binary, targetBinary );
            std::vector<bool> existed( files.size() );
            size_t installed = 0;

            if ( cancel->load() )
            {
                throw std::runtime_error( "Update cancelled" );
            }

            for ( size_t index = 0; index < files.size(); ++index )
            {
                const auto& destination = files[index].second;
                fs::create_directories( destination.parent_path() );

                if ( fs::is_symlink( destination ) )
                {
                    throw std::runtime_error( "Automatic updates do not replace symlinked plugin files" );
                }

                existed[index] = fs::exists( destination );

                if ( existed[index] )
                {
                    fs::copy_file( destination, destination.string() + ".previous", fs::copy_options::overwrite_existing );
                    fs::permissions( files[index].first, fs::status( destination ).permissions() );
                }
            }

            try
            {
                for ( const auto& file : files )
                {
                    fs::rename( file.first, file.second );
                    ++installed;
                }
            }
            catch ( ... )
            {
                while ( installed > 0 )
                {
                    --installed;
                    const auto& destination = files[installed].second;

                    if ( existed[installed] )
                    {
                        fs::rename( destination.string() + ".previous", destination );
                    }
                    else
                    {
                        fs::remove( destination );
                    }
                }

                throw;
            }

            return { true, "Installed release " + release->version + " and " + std::to_string( mapNames.size() ) +
                               " VIS maps, restart the server to activate it. Previous files retained as .previous" };
        }
        catch ( const fs::filesystem_error& )
        {
            return { false, "Update failed, could not access plugin files. Check write permissions" };
        }
        catch ( const std::exception& error )
        {
            return { false, "Update failed, " + std::string( error.what() ) };
        }
    }
}
