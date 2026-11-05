#pragma once
#include <cstdlib>
#include <cstring>

inline bool steam_video_env_on(const char* k) {
    const char* v = std::getenv(k);
    return v && *v && std::strcmp(v, "0") != 0;
}

inline bool steam_prefer_x11() {
    const bool steam = steam_video_env_on("SteamGameId") || steam_video_env_on("SteamAppId") ||
                       steam_video_env_on("SteamDeck");
    const char* chosen = std::getenv("SDL_VIDEO_DRIVER");
    if (!steam || (chosen && *chosen) || !steam_video_env_on("DISPLAY")) return false;
    return setenv("SDL_VIDEO_DRIVER", "x11,wayland", 0) == 0;
}
