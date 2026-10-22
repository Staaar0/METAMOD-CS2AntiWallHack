#include <cstdlib>
#include <new>

namespace
{
    void* Allocate( std::size_t size, std::size_t alignment = 0 )
    {
        if ( !size )
        {
            size = 1;
        }

        for ( ;; )
        {
            void* memory = nullptr;

            if ( !alignment )
            {
                memory = std::malloc( size );
            }
            else
            {
                if ( alignment < sizeof( void* ) )
                {
                    alignment = sizeof( void* );
                }

                if ( posix_memalign( &memory, alignment, size ) != 0 )
                {
                    memory = nullptr;
                }
            }

            if ( memory )
            {
                return memory;
            }

            if ( auto handler = std::get_new_handler() )
            {
                handler();
            }
            else
            {
                throw std::bad_alloc();
            }
        }
    }
}

void* operator new( std::size_t size )
{
    return Allocate( size );
}

void* operator new[]( std::size_t size )
{
    return Allocate( size );
}

void operator delete( void* memory ) noexcept
{
    std::free( memory );
}

void operator delete[]( void* memory ) noexcept
{
    std::free( memory );
}

void operator delete( void* memory, std::size_t ) noexcept
{
    std::free( memory );
}

void operator delete[]( void* memory, std::size_t ) noexcept
{
    std::free( memory );
}

void* operator new( std::size_t size, const std::nothrow_t& ) noexcept
{
    try
    {
        return Allocate( size );
    }
    catch ( ... )
    {
        return nullptr;
    }
}

void* operator new[]( std::size_t size, const std::nothrow_t& ) noexcept
{
    try
    {
        return Allocate( size );
    }
    catch ( ... )
    {
        return nullptr;
    }
}

void operator delete( void* memory, const std::nothrow_t& ) noexcept
{
    std::free( memory );
}

void operator delete[]( void* memory, const std::nothrow_t& ) noexcept
{
    std::free( memory );
}

void* operator new( std::size_t size, std::align_val_t alignment )
{
    return Allocate( size, static_cast<std::size_t>( alignment ) );
}

void* operator new[]( std::size_t size, std::align_val_t alignment )
{
    return Allocate( size, static_cast<std::size_t>( alignment ) );
}

void operator delete( void* memory, std::align_val_t ) noexcept
{
    std::free( memory );
}

void operator delete[]( void* memory, std::align_val_t ) noexcept
{
    std::free( memory );
}

void operator delete( void* memory, std::size_t, std::align_val_t ) noexcept
{
    std::free( memory );
}

void operator delete[]( void* memory, std::size_t, std::align_val_t ) noexcept
{
    std::free( memory );
}

void* operator new( std::size_t size, std::align_val_t alignment, const std::nothrow_t& ) noexcept
{
    try
    {
        return Allocate( size, static_cast<std::size_t>( alignment ) );
    }
    catch ( ... )
    {
        return nullptr;
    }
}

void* operator new[]( std::size_t size, std::align_val_t alignment, const std::nothrow_t& ) noexcept
{
    try
    {
        return Allocate( size, static_cast<std::size_t>( alignment ) );
    }
    catch ( ... )
    {
        return nullptr;
    }
}

void operator delete( void* memory, std::align_val_t, const std::nothrow_t& ) noexcept
{
    std::free( memory );
}

void operator delete[]( void* memory, std::align_val_t, const std::nothrow_t& ) noexcept
{
    std::free( memory );
}
