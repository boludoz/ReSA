/*
    Plugin-SDK file
    Authors: GTA Community. See more here
    https://github.com/DK22Pac/plugin-sdk
    Do not delete this comment block. Respect others' work!
*/
#pragma once

#include "Base.h"
#include "PedGroup.h"

class CPed;

class CPedGroups {
public:
    static inline std::array<uint16, 8>& ScriptReferenceIndex = *reinterpret_cast<std::array<uint16, 8>*>(0xC098D0);
    static inline std::array<char, 8>& ms_activeGroups = *reinterpret_cast<std::array<char, 8>*>(0xC098E0);
    static inline bool& ms_bIsPlayerOnAMission = *reinterpret_cast<bool*>(0xC098E8);
    static inline uint32& ms_iNoOfPlayerKills = *reinterpret_cast<uint32*>(0xC098EC);
    static inline std::array<CPedGroup, 8>& ms_groups = *reinterpret_cast<std::array<CPedGroup, 8>*>(0xC09920);

public:
    static void InjectHooks();

    static int32 AddGroup();
    static void RemoveGroup(int32 groupID);
    static void RemoveAllFollowersFromGroup(int32 groupId);

    static void Init();

    static void RegisterKillByPlayer();

    static void CleanUpForShutDown();

    static bool IsGroupLeader(CPed* ped);

    static CPedGroup* GetPedsGroup(const CPed* ped);
    static int32 GetGroupId(const CPedGroup* pedGroup);

    static void Process();

    static bool AreInSameGroup(const CPed* ped1, const CPed* ped2);
    static bool IsInPlayersGroup(CPed* ped);

    // inlined
    static CPedGroup& GetGroup(int32 groupId);

    static auto GetActiveGroupsWithIDs() {
        return ms_groups
            | rngv::enumerate
            | rngv::filter([](auto&& p) { return ms_activeGroups[std::get<0>(p)]; });
    }
};
