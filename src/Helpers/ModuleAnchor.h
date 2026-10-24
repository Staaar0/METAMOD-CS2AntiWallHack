#pragma once

namespace awh
{
    inline void* InterfaceModuleAnchor( void* instance )
    {
        return instance ? *static_cast<void***>( instance ) : nullptr;
    }
}
