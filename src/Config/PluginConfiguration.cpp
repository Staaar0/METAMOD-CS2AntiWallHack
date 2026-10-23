#include "Plugin.h"

Json AntiWallHack::ReadGamedata( const fs::path& path )
{
    std::ifstream file( path );

    if ( !file )
    {
        throw std::runtime_error( "Cannot open " + path.string() );
    }

    return ParseGamedata( file );
}

void AntiWallHack::LoadSettings()
{
    if ( !fs::exists( configPath ) )
    {
        fs::create_directories( configPath.parent_path() );
        std::ofstream out( configPath );
        out << DefaultConfig;
        out.close();

        if ( !out )
        {
            throw std::runtime_error( "Cannot create " + configPath.string() );
        }
    }

    std::ifstream file( configPath );

    if ( !file )
    {
        throw std::runtime_error( "Cannot open " + configPath.string() );
    }

    settings = ParseConfig( file );
}

void AntiWallHack::ConfigureConvar()
{
    ConVarRefAbstract cvar( "sv_enable_donttransmit" );

    if ( !cvar.IsValidRef() )
    {
        return;
    }

    if ( !convarChanged )
    {
        previousConvar = cvar.GetString().Get();
        convarChanged = true;
    }

    cvar.SetBool( false );
}
