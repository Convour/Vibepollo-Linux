# linux specific target definitions

# Using newer c++ compilers / features on older distros causes runtime dyn link errors
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        -static-libgcc
        -static-libstdc++
)

# Steam-sync companion launcher: the process Sunshine actually tracks in place of a bare
# `steam://rungameid/<appid>` command, so quitting the game ends the stream and ending the
# stream kills the game (see src/platform/linux/steam_library.cpp and the launcher's own
# file comment for why a proxy process is required here).
add_executable(sunshine-steam-launcher "${CMAKE_SOURCE_DIR}/tools/steam_launcher/main.cpp")
set_target_properties(sunshine-steam-launcher PROPERTIES CXX_STANDARD 23)
target_compile_options(sunshine-steam-launcher PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
