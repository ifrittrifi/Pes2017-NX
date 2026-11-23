#include "spawn.h"

#if __has_include(<SDL3/SDL.h>)

namespace
{

    void Connect(SDL_PropertiesID properties, const char* stream, bool has)
    {
        SDL_SetNumberProperty(properties, stream, has ? SDL_PROCESS_STDIO_INHERITED : SDL_PROCESS_STDIO_NULL);
    }
}

SDL_Process* spawn::Start(const char* const* args, SDL_Environment* environment)
{
    SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<char**>(args));
    if (environment) SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    return process;
}
#endif
