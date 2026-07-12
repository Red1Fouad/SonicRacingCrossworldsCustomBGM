#pragma once

#define SHARED_MEMORY_NAME "SonicCBGM"

#pragma pack(push, 1)
struct SharedMemory {
    volatile LONG cmdReady;
    int command;
    char cueName[256];
    int poolId;
    bool allowLoop;

    char currentTrack[256];
    char poolName[32];
    float volume;
    bool isPlaying;
    bool active;
    int trackCount;
    int lobbyCount;
    int titleCount;
    bool hasBgmTracks;
    bool hasLobbyTracks;
    bool hasTitleTracks;
};
#pragma pack(pop)
