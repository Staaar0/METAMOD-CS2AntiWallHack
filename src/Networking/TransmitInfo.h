#pragma once
#include <engine2/isource2server.h>
#include <playerslot.h>
#include <tier0/utlvector.h>
#include <cstddef>

// The second mask carries deletion deltas, it is not an always-transmit mask.
struct VisibilityInfo
{
    uint32 size;
    SpawnGroupHandle_t spawnGroup;
    CBitVec<4096> bits;
};

struct TransmitInfo
{
    CBitVec<16384>* entities;
    CBitVec<16384>* deletion;
    CBitVec<16384>* outOfPVS;
    CBitVec<16384>* always;
    CUtlVector<SpawnGroupHandle_t> loadedSpawnGroups;
    VisibilityInfo visibility;
    CPlayerSlot slot;
    bool fullUpdate;
};

static_assert( sizeof( VisibilityInfo ) == 520 );
static_assert( sizeof( TransmitInfo ) == 584 );
static_assert( offsetof( TransmitInfo, slot ) == 0x240 );
static_assert( offsetof( TransmitInfo, fullUpdate ) == 0x244 );

static_assert( sizeof( TransmitInfo ) == sizeof( CCheckTransmitInfo ) );
static_assert( offsetof( TransmitInfo, slot ) == offsetof( CCheckTransmitInfo, m_nPlayerSlot ) );
static_assert( offsetof( TransmitInfo, fullUpdate ) == offsetof( CCheckTransmitInfo, m_bFullUpdate ) );
