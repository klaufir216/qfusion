#include "AasRouteCache.h"
#include "AasElementsMask.h"
#include "../static_vector.h"
#include "../ai_local.h"
#include "../bot.h"

#include "../../../qalgo/Links.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <cstdlib>
#include <algorithm>
#include <limits>
#include <cmath>

template <typename T> inline T *CastCheckingAlignment( void *ptr ) {
	assert( !( ( (uintptr_t)ptr ) % alignof( T ) ) );
	return (T *)ptr;
}

static inline uint16_t ToUint16CheckingRange( int value ) {
	assert( (unsigned)value <= std::numeric_limits<uint16_t>::max() );
	return (uint16_t)value;
}

// Static member definition
AiAasRouteCache *AiAasRouteCache::shared = nullptr;

// TODO: We can and should eliminate access to this lookup table
// along with necessity to maintain it
// if AAS file representation is decoupled from the memory one
static int travelFlagForType[MAX_TRAVELTYPES];

void AiAasRouteCache::Init( const AiAasWorld &aasWorld ) {
	if( shared ) {
		AI_FailWith( "AiAasRouteCache::Init()", "The shared instance is already present\n" );
	}

	// Prepare the lookup table
	InitTravelFlagFromType();

	// AiAasRouteCache is quite large, so it should be allocated on heap
	shared = (AiAasRouteCache *)G_Malloc( sizeof( AiAasRouteCache ) );
	new(shared) AiAasRouteCache( *AiAasWorld::Instance() );
}

void AiAasRouteCache::Shutdown() {
	// This may be called on first map load when an instance has never been instantiated
	if( shared ) {
		shared->~AiAasRouteCache();
		G_Free( shared );
		// Allow the pointer to be reused, otherwise an assertion will fail on a next Init() call
		shared = nullptr;
	}
}

AiAasRouteCache *AiAasRouteCache::NewInstance( const int *travelFlags_ ) {
	return new( G_Malloc( sizeof( AiAasRouteCache ) ) )AiAasRouteCache( Shared(), travelFlags_ );
}

void AiAasRouteCache::ReleaseInstance( AiAasRouteCache *instance ) {
	if( instance == Shared() ) {
		AI_FailWith( "AiAasRouteCache::ReleaseInstance()", "Attempt to release the shared instance\n" );
	}

	instance->~AiAasRouteCache();
	G_Free( instance );
}

static const int DEFAULT_TRAVEL_FLAGS[] = { Bot::PREFERRED_TRAVEL_FLAGS, Bot::ALLOWED_TRAVEL_FLAGS };

AiAasRouteCache::AiAasRouteCache( const AiAasWorld &aasWorld_ )
	: travelFlags( DEFAULT_TRAVEL_FLAGS ), aasWorld( aasWorld_ ) {
	InitCompactReachDataAreaDataAndHelpers();

	InitPathFindingNodes();

	CreateReversedReach();

	InitClusterAreaCache();
	InitPortalCache();

	CalculateAreaTravelTimes();
	InitPortalMaxTravelTimes();

	loaded = true;
}

AiAasRouteCache::AiAasRouteCache( AiAasRouteCache *parent, const int *newTravelFlags )
	: travelFlags( newTravelFlags ), aasWorld( parent->aasWorld ), loaded( true ) {
	InitPathFindingNodes();

	// A ref counter is shared for aasRevReach and aasRevLinks
	// as they are allocated within a single memory chunk
	// and aasRevReach is at the beginning of that chunk.
	aasRevReach = AddRef( parent->aasRevReach );
	// Just copy the address.
	aasRevLinks = parent->aasRevLinks;

	// Create a new mutable copy of data based on the parent's one
	areaPathFindingData = parent->CloneAreaPathFindingData();

	// These items share the buffer as well, refer to the explanation above
	reachPathFindingData = AddRef( parent->reachPathFindingData );

	InitClusterAreaCache();
	InitPortalCache();

	areaTravelTimes = AddRef( parent->areaTravelTimes );
	portalMaxTravelTimes = AddRef( parent->portalMaxTravelTimes );
}

AiAasRouteCache::~AiAasRouteCache() {
	if( !loaded ) {
		return;
	}

	FreeAllClusterAreaCache();
	FreeAllPortalCache();

	FreeRefCountedMemory( areaTravelTimes );
	FreeRefCountedMemory( portalMaxTravelTimes );
	FreeRefCountedMemory( aasRevReach );

	FreeMemory( areaPathFindingNodes );
	FreeMemory( portalPathFindingNodes );

	FreeRefCountedMemory( reachPathFindingData );
	FreeMemory( areaPathFindingData );

	FreeAreaAndPortalMemoryPools();
}

/**
 * We have switched to passing AAS arrays explicitly as arguments to avoid hidden costs of pointer chasing
 */
inline int ClusterAreaNum( const aas_areasettings_t *aasAreaSettings,
						   const aas_portal_t *aasPortals,
						   int cluster, int areaNum ) {
	const auto &areaSettings = aasAreaSettings[areaNum];
	const int areaCluster = areaSettings.cluster;
	if( areaCluster > 0 ) {
		return areaSettings.clusterareanum;
	}

	const auto &portal = aasPortals[-areaCluster];
	const int side = portal.frontcluster != cluster;
	return portal.clusterareanum[side];
}

void AiAasRouteCache::InitTravelFlagFromType() {
	for( int &flag: travelFlagForType ) {
		flag = TFL_INVALID;
	}

	travelFlagForType[TRAVEL_INVALID] = TFL_INVALID;
	travelFlagForType[TRAVEL_WALK] = TFL_WALK;
	travelFlagForType[TRAVEL_CROUCH] = TFL_CROUCH;
	travelFlagForType[TRAVEL_BARRIERJUMP] = TFL_BARRIERJUMP;
	travelFlagForType[TRAVEL_JUMP] = TFL_JUMP;
	travelFlagForType[TRAVEL_LADDER] = TFL_LADDER;
	travelFlagForType[TRAVEL_WALKOFFLEDGE] = TFL_WALKOFFLEDGE;
	travelFlagForType[TRAVEL_SWIM] = TFL_SWIM;
	travelFlagForType[TRAVEL_WATERJUMP] = TFL_WATERJUMP;
	travelFlagForType[TRAVEL_TELEPORT] = TFL_TELEPORT;
	travelFlagForType[TRAVEL_ELEVATOR] = TFL_ELEVATOR;
	travelFlagForType[TRAVEL_ROCKETJUMP] = TFL_ROCKETJUMP;
	travelFlagForType[TRAVEL_BFGJUMP] = TFL_BFGJUMP;
	travelFlagForType[TRAVEL_GRAPPLEHOOK] = TFL_GRAPPLEHOOK;
	travelFlagForType[TRAVEL_DOUBLEJUMP] = TFL_DOUBLEJUMP;
	travelFlagForType[TRAVEL_RAMPJUMP] = TFL_RAMPJUMP;
	travelFlagForType[TRAVEL_STRAFEJUMP] = TFL_STRAFEJUMP;
	travelFlagForType[TRAVEL_JUMPPAD] = TFL_JUMPPAD;
	travelFlagForType[TRAVEL_FUNCBOB] = TFL_FUNCBOB;
}

void AiAasRouteCache::UnlinkCache( AreaOrPortalCacheTable *cache ) {
	if( cache->time_next ) {
		cache->time_next->time_prev = cache->time_prev;
	} else {
		newestCache = cache->time_prev;
	}
	if( cache->time_prev ) {
		cache->time_prev->time_next = cache->time_next;
	} else {
		oldestCache = cache->time_next;
	}
	cache->time_next = nullptr;
	cache->time_prev = nullptr;
}

void AiAasRouteCache::LinkCache( AreaOrPortalCacheTable *cache ) {
	if( newestCache ) {
		newestCache->time_next = cache;
		cache->time_prev = newestCache;
	} else {
		oldestCache = cache;
		cache->time_prev = nullptr;
	}
	cache->time_next = nullptr;
	newestCache = cache;
}

void AiAasRouteCache::FreeRoutingCache( AreaOrPortalCacheTable *cache ) {
	UnlinkCache( cache );
	FreeAreaAndPortalCacheMemory( cache );
}

void AiAasRouteCache::SetDisabledZones( DisableZoneRequest **requests, int numRequests ) {
	// Copy the reference to a local var for faster access
	AreaPathFindingData *const __restrict areaPathFindingData = this->areaPathFindingData;

	const auto numAreas = aasWorld.NumAreas();

	// First, save old area statuses and set new ones as non-blocked
	for( int i = 0; i < numAreas; ++i ) {
		areaPathFindingData[i].disabledStatus.ShiftCurrToOldStatus();
	}

	auto *const __restrict blockedAreasTable = AasElementsMask::BlockedAreasTable();
	memset( blockedAreasTable, 0, numAreas * sizeof( bool ) );

	for( int i = 0; i < numRequests; ++i ) {
		requests[i]->FillBlockedAreasTable( blockedAreasTable );
	}

	// For each selected area mark area as disabled
	for( int i = 0; i < numAreas; ++i ) {
		if( blockedAreasTable[i] ) {
			areaPathFindingData[i].disabledStatus.SetCurrStatus( true );
		}
	}

	// For each area compare its old and new status
	bool shouldClearCache = false;
	for( int i = 0; i < numAreas; ++i ) {
		const auto &status = areaPathFindingData[i].disabledStatus;
		// TODO: We can test multiple statuses using SIMD
		if( status.OldStatus() != status.CurrStatus() ) {
			shouldClearCache = true;
			break;
		}
	}

	if( !shouldClearCache ) {
		return;
	}

	ResetAllClusterAreaCache();
	ResetAllPortalCache();

	newestCache = nullptr;
	oldestCache = nullptr;

	resultCache.Clear();
}

void AiAasRouteCache::InitCompactReachDataAreaDataAndHelpers() {
	const int numAreas = aasWorld.NumAreas();
	const int numReach = aasWorld.NumReach();

	static_assert( alignof( ReachPathFindingData ) < alignof( int ), "Alignment assumptions are broken" );
	// We should check this as long as AAS world code does not operate on types of guaranteed fixed size
	static_assert( sizeof( int ) == sizeof( int32_t ), "Assumptions on int being 32 bit are broken" );

	size_t reachDataSize = numReach * sizeof( ReachPathFindingData );
	reachPathFindingData = (ReachPathFindingData *)GetClearedRefCountedMemory( reachDataSize );

	size_t areaDataSize = numAreas * sizeof( AreaPathFindingData );
	areaPathFindingData = (AreaPathFindingData *)GetClearedMemory( areaDataSize );

	const auto *const aasAreaSettings = aasWorld.AreaSettings();
	const auto *const aasPortals = aasWorld.Portals();
	for( int i = 0; i < numAreas; ++i ) {
		const auto &areaSettings = aasAreaSettings[i];
		int contents = areaSettings.contents;
		int tfl = 0;
		if( contents & AREACONTENTS_WATER ) {
			tfl |= TFL_WATER;
		} else if( contents & AREACONTENTS_SLIME ) {
			tfl |= TFL_SLIME;
		} else if( contents & AREACONTENTS_LAVA ) {
			tfl |= TFL_LAVA;
		} else {
			tfl |= TFL_AIR;
		}
		if( contents & AREACONTENTS_DONOTENTER ) {
			tfl |= TFL_DONOTENTER;
		}
		if( contents & AREACONTENTS_NOTTEAM1 ) {
			tfl |= TFL_NOTTEAM1;
		}
		if( contents & AREACONTENTS_NOTTEAM2 ) {
			tfl |= TFL_NOTTEAM2;
		}
		if( areaSettings.areaflags & AREA_BRIDGE ) {
			tfl |= TFL_BRIDGE;
		}

		auto *const areaData = &areaPathFindingData[i];
		areaData->contentsTravelFlags = tfl;
		areaData->settingsAreaFlags = areaSettings.areaflags;
		areaData->firstReachNum = ToUint16CheckingRange( areaSettings.firstreachablearea );
		int clusterOrPortalNum = areaSettings.cluster;
		assert( clusterOrPortalNum >= std::numeric_limits<int8_t>::min() );
		assert( clusterOrPortalNum <= std::numeric_limits<int8_t>::max() );
		areaData->clusterOrPortalNum = (int8_t)clusterOrPortalNum;
		int clusterAreaNum = ClusterAreaNum( aasAreaSettings, aasPortals, clusterOrPortalNum, i );
		areaData->clusterAreaNum = ToUint16CheckingRange( clusterAreaNum );
		areaData->disabledStatus = AreaDisabledStatus();
	}

	const auto *const aasReach = aasWorld.Reachabilities();
	for( int i = 0; i < numReach; ++i ) {
		const auto &reach = aasReach[i];
		auto *const &reachData = &reachPathFindingData[i];
		reachData->travelFlags = ::travelFlagForType[reach.traveltype & TRAVELTYPE_MASK];
		reachData->travelTime = reach.traveltime;
	}
}

AiAasRouteCache::AreaPathFindingData *AiAasRouteCache::CloneAreaPathFindingData() {
	const int numAreas = aasWorld.NumAreas();
	size_t dataSize = numAreas * sizeof( AreaPathFindingData );
	auto *const newData = (AreaPathFindingData *)GetClearedMemory( dataSize );
	assert( this->areaPathFindingData );
	memcpy( newData, this->areaPathFindingData, dataSize );
	// Reset area routing status
	for( int i = 0; i < numAreas; ++i ) {
		newData[i].disabledStatus.SetCurrStatus( false );
	}
	return newData;
}

void AiAasRouteCache::CreateReversedReach() {
	const auto revReachSize = aasWorld.NumAreas() * sizeof( RevReach );
	const auto revLinkSize = aasWorld.NumReachabilities() * sizeof( RevLink );
	auto *ptr = (uint8_t *)GetClearedRefCountedMemory( revReachSize + revLinkSize );

	this->aasRevReach = CastCheckingAlignment<RevReach>( ptr );
	ptr += revReachSize;
	this->aasRevLinks = CastCheckingAlignment<RevLink>( ptr );

	const auto *const aasReach = aasWorld.Reachabilities();
	const auto *const aasAreaSettings = aasWorld.AreaSettings();

	for( int i = 1, end = aasWorld.NumAreas(); i < end; ++i ) {
		const auto &areaSettings = aasAreaSettings[i];
		int numReachableAreas = areaSettings.numreachableareas;
		if( numReachableAreas >= 128 ) {
			G_Printf( S_COLOR_YELLOW "area %d has more than 128 reachabilities\n", i );
			numReachableAreas = 128;
		}

		// Create reversed links for the reachabilities
		for( int n = 0; n < numReachableAreas; n++ ) {
			const auto *reach = aasReach + areaSettings.firstreachablearea + n;

			auto *revLink = CastCheckingAlignment<RevLink>( ptr );

			revLink->areaNum = ToUint16CheckingRange( i );
			revLink->linkNum = ToUint16CheckingRange( areaSettings.firstreachablearea + n );
			revLink->nextLink = aasRevReach[reach->areanum].firstRevLink;
			aasRevReach[reach->areanum].firstRevLink = ToUint16CheckingRange( (int)( revLink - aasRevLinks ) );
			aasRevReach[reach->areanum].numLinks++;

			ptr += sizeof( RevLink );
		}
	}
}

#define PAD( base, alignment ) ( ( ( base ) + ( alignment ) - 1 ) & ~( ( alignment ) - 1 ) )

void AiAasRouteCache::CalculateAreaTravelTimes() {
	const auto *const aasAreaSettings = aasWorld.AreaSettings();
	const auto *const aasReach = aasWorld.Reachabilities();
	const auto numAreas = aasWorld.NumAreas();

	// "area travel times" is a 3-dimensional array (is viewed as a 3-dimensional array).
	// The outer index is addressed by area numbers.
	// The middle index is addressed by a relative number of a reachability
	// starting from first for the area settings for the area.
	// The inner index is addressed by a number of a link in reversed reachabilities chain for the reachability.
	size_t size = numAreas * sizeof( uint16_t ** );
	for( int i = 0; i < numAreas; i++ ) {
		const auto &revReach = aasRevReach[i];
		const int numReachAreas = aasAreaSettings[i].numreachableareas;
		size += numReachAreas * sizeof( uint16_t * );
		size += numReachAreas * PAD( revReach.numLinks, sizeof( void * ) ) * sizeof( uint16_t );
	}

	auto *ptr = (uint8_t *)GetClearedRefCountedMemory( size );
	areaTravelTimes = (uint16_t ***) ptr;

	ptr += numAreas * sizeof( uint16_t ** );

	for( int i = 0; i < numAreas; i++ ) {
		const auto &areaSettings = aasAreaSettings[i];

		// This array is addressed by a relative number of reachability
		uint16_t **const areaReachTravelTimes = areaTravelTimes[i] = CastCheckingAlignment<uint16_t *>( ptr );
		ptr += areaSettings.numreachableareas * sizeof( uint16_t * );

		// This is a head of reversed reachabilities chain for the area
		const auto &revReach = aasRevReach[i];
		for( int l = 0; l < areaSettings.numreachableareas; l++ ) {
			uint16_t *const linkTravelTimes = areaReachTravelTimes[l] = CastCheckingAlignment<uint16_t>( ptr );
			ptr += PAD( revReach.numLinks, sizeof( void * ) ) * sizeof( uint16_t );

			const auto &reach = aasReach[areaSettings.firstreachablearea + l];
			int revLinkNum = revReach.firstRevLink;
			const RevLink *revLink;
			for( int n = 0; n < revReach.numLinks; revLinkNum = revLink->nextLink, n++ ) {
				revLink = aasRevLinks + revLinkNum;
				// This is an inlined and modified body of old AreaTravelTime() call
				// The old comment says:
				// "travel time in hundredths of a second = distance * 100 / speed"
				// The old code used to take a distance as a base and apply "distance factors"
				// for 3 different kinds of movement: walking, swimming and crouching.
				// Crouching detection was based on "area presence type"
				// which is completely wrong set by the AAS compiler
				// (every area that has a small height has other presence type than "normal"
				// even if no crouching is really required (there is an void area above).
				// This used to lead to producing incorrect time estimations.
				float distance = sqrtf( DistanceSquared( aasReach[revLink->linkNum].end, reach.start ) );
				// Apply "distance factors" tweaked for actual average bot movement speed.
				if( !( areaSettings.areaflags & AREA_LIQUID ) ) {
					distance *= 0.23f;
				} else {
					distance *= 1.00f;
				}
				const auto intDistance = (int)distance;
				if( intDistance > 1 ) {
					linkTravelTimes[n] = ToUint16CheckingRange( intDistance );
					continue;
				}
				// Set to the minimal feasible AAS travel time
				linkTravelTimes[n] = 1;
			}
		}
	}
}

void AiAasRouteCache::InitPortalMaxTravelTimes() {
	const auto *const aasAreaSettings = aasWorld.AreaSettings();
	const auto *const aasPortals = aasWorld.Portals();

	const auto numPortals = aasWorld.NumPortals();

	portalMaxTravelTimes = (int *)GetClearedRefCountedMemory( numPortals * sizeof( int ) );

	for( int portalNum = 0; portalNum < numPortals; portalNum++ ) {
		const auto &portal = aasPortals[portalNum];
		const auto portalAreaNum = portal.areanum;
		// Reversed reach. of this portal area
		const auto &revReach = &aasRevReach[portalAreaNum];
		const auto numReachAreas = aasAreaSettings[portalAreaNum].numreachableareas;

		int maxTime = 0;
		for( int l = 0; l < numReachAreas; l++ ) {
			const RevLink *revLink;
			int revLinkNum = revReach->firstRevLink;
			for( int n = 0; n < revReach->numLinks; revLinkNum = revLink->nextLink, n++ ) {
				revLink = aasRevLinks + revLinkNum;
				int t = areaTravelTimes[portalAreaNum][l][n];
				if( t > maxTime ) {
					maxTime = t;
				}
			}
		}

		portalMaxTravelTimes[portalNum] = maxTime;
	}
}

class FreelistPool {
public:
	/**
	 * A header that is put at a beginning of a block before actual user-visible data.
	 * Links are arrays so generic {@code ::Link()/::Unlink()} facilities can be used.
	 */
	struct BlockHeader {
		BlockHeader *prev[1];
		BlockHeader *next[1];
	};

	static_assert( !( sizeof( BlockHeader ) % 8 ), "BlockHeader size is assumed to be a multiple of 8" );
private:
	// Freelist head
	BlockHeader *freeBlock { nullptr };
	// Actual blocks data
	uint8_t *buffer;

	// An actual blocks data size and a maximal count of blocks.
	const size_t blockSize, maxBlocks;
	size_t blocksInUse { 0 };
public:
	FreelistPool( void *buffer_, size_t bufferSize, size_t blockSize_ );
	virtual ~FreelistPool() = default;

	void *Alloc( size_t size );
	void Free( void *ptr );

	inline bool MayOwn( const void *ptr ) const {
		const uint8_t *comparablePtr = (const uint8_t *)ptr;
		const uint8_t *bufferBoundary = buffer + ( blockSize + sizeof( BlockHeader ) ) * ( maxBlocks );
		return buffer <= comparablePtr && comparablePtr <= bufferBoundary;
	}

	inline size_t Size() const { return blocksInUse; }
	inline size_t Capacity() const { return maxBlocks; }
};

class AreaAndPortalCacheAllocatorBin {
	FreelistPool *freelistPool { nullptr };

	void *usedSingleBlock { nullptr };
	void *freeSingleBlock { nullptr };

	const unsigned numBlocks;
	const unsigned blockSize;

	// Sets the reference in the chunk to its owner so the block can destruct self
	inline void *SetSelfAsTag( void *block ) {
		// Check the block alignment
		// We have to always return 8-byte aligned blocks to follow the malloc contract
		// regardless of address size, so 8 bytes are wasted anyway
		uint64_t *u = (uint64_t *)block;
		u[0] = (uintptr_t)this;
		return u + 1;
	}

	// Don't call directly, use FreeTaggedBlock() instead.
	// A real raw block pointer is expected (that is legal to be fed to G_Free),
	// and Alloc() returns pointers shifted by 8 (a tag is prepended)
	void Free( void *ptr );
public:
	AreaAndPortalCacheAllocatorBin *next { nullptr };

	static unsigned SuggestNumberOfBlocks() {
		// ( 2^16 - 1 ) is the maximum supported number of reachabilities (we use short indices)
		auto frac = AiAasWorld::Instance()->NumReach() / (float)std::numeric_limits<uint16_t>::max();
		// Requirements do not grow linear. This will be a better approximation.
		frac = std::sqrt( frac );
		return (unsigned)( 512.0f + 768.0f * frac );
	}

	AreaAndPortalCacheAllocatorBin( size_t blockSize_, size_t numBlocks_ = SuggestNumberOfBlocks() )
		: numBlocks( (unsigned)numBlocks_ ), blockSize( (unsigned)blockSize_ ) {}

	~AreaAndPortalCacheAllocatorBin() {
		if( usedSingleBlock ) {
			G_Free( usedSingleBlock );
		}
		if( freeSingleBlock ) {
			G_Free( freeSingleBlock );
		}
		if( freelistPool ) {
			freelistPool->~FreelistPool();
			G_Free( freelistPool );
		}
	}

	void *Alloc( size_t size );

	bool FitsSize( size_t size ) const {
		// Use a strict comparison and do not try to reuse a bin for blocks of different size.
		// There are usually very few size values.
		// Moreover bins for small sizes are addressed by the size.
		// If we try reusing the same bin for different sizes, the cache gets evicted way too often.
		// (these small bins usually correspond to cluster cache).
		return size == blockSize;
	}

	bool NeedsCleanup() const {
		if( !freelistPool ) {
			return false;
		}
		// Raising the threshold leads to pool exhaustion on some maps
		return freelistPool->Size() / (float)freelistPool->Capacity() > 0.66f;
	}

	static void FreeTaggedBlock( void *ptr ) {
		// Check alignment of the supplied pointer
		assert( !( (uintptr_t)ptr % 8 ) );
		// Real block starts 8 bytes below
		uint64_t *realBlock = ( (uint64_t *)ptr ) - 1;
		// Extract the bin stored as a tag
		auto *bin = (AreaAndPortalCacheAllocatorBin *)( (uintptr_t)realBlock[0] );
		// Free the real block registered by the bin
		bin->Free( realBlock );
	}
};

FreelistPool::FreelistPool( void *buffer_, size_t bufferSize, size_t blockSize_ )
	: buffer( (uint8_t *)buffer_ )
	, blockSize( blockSize_ )
	, maxBlocks( bufferSize / ( blockSize_ + sizeof( BlockHeader ) ) ) {
#ifndef PUBLIC_BUILD
	if( ( (uintptr_t)buffer ) & 7 ) {
		AI_FailWith( "FreelistPool::FreelistPool()", "Illegal buffer pointer (should be at least 8-byte-aligned)\n" );
	}
	if( blockSize & 7u ) {
		AI_FailWith( "FreelistPool::FreelistPool()", "Illegal block size (must be a multiple of 8)\n" );
	}
#endif

	freeBlock = nullptr;
	if( !maxBlocks ) {
		return;
	}

	auto *currBlock = (BlockHeader *)this->buffer;
	freeBlock = currBlock;
	if( maxBlocks == 1 ) {
		currBlock->prev[0] = nullptr;
		currBlock->next[0] = nullptr;
		return;
	}

	// We can't use array access on BlockHeader * pointer
	// since a real block size is not a sizeof(BlockHeader).
	// A next block has this offset in bytes from previous one:
	const size_t stride = blockSize + sizeof( BlockHeader );
	uint8_t *nextBlockPtr = this->buffer + stride;
	auto *nextBlock = (BlockHeader *)nextBlockPtr;
	// Setup links in free list for the first block
	currBlock->prev[0] = nullptr;
	currBlock->next[0] = nextBlock;

	auto *prevBlock = currBlock;
	currBlock = nextBlock;
	// Setup links for blocks [1, maxBlocks - 1)
	for( unsigned i = 1; i < maxBlocks - 1; ++i ) {
		nextBlockPtr += stride;
		nextBlock = (BlockHeader *)nextBlockPtr;
		assert( (uint8_t *)currBlock - (uint8_t *)prevBlock == (ptrdiff_t)stride );
		assert( (uint8_t *)nextBlock - (uint8_t *)currBlock == (ptrdiff_t)stride );
		currBlock->prev[0] = prevBlock;
		currBlock->next[0] = nextBlock;
		prevBlock = currBlock;
		currBlock = nextBlock;
	}

	// currBlock must point at the last block
	assert( (void *)currBlock == (void *)( this->buffer + stride * ( maxBlocks - 1 ) ) );
	assert( (void *)prevBlock == (void *)( this->buffer + stride * ( maxBlocks - 2 ) ) );
	currBlock->prev[0] = prevBlock;
	currBlock->next[0] = nullptr;
}

void *FreelistPool::Alloc( size_t size ) {
#ifndef PUBLIC_BUILD
	if( size > blockSize ) {
		constexpr const char *format = "Attempt to allocate more bytes %u than the block size %u\n";
		AI_FailWith( "FreelistPool::Alloc()", format, (unsigned)size, (unsigned)blockSize );
	}

	if( !freeBlock ) {
		AI_FailWith( "FreelistPool::Alloc()", "There are no free blocks left\n" );
	}
#endif

	BlockHeader *const block = Unlink( freeBlock, &freeBlock, 0 );
	++blocksInUse;
	// Return a pointer to a datum after the header
	return block + 1;
}

void FreelistPool::Free( void *ptr ) {
	BlockHeader *const block = ( (BlockHeader *)ptr ) - 1;
	Link( block, &freeBlock, 0 );
	--blocksInUse;
}

void *AreaAndPortalCacheAllocatorBin::Alloc( size_t size ) {
#ifndef PUBLIC_BUILD
	if( size > blockSize ) {
		const char *message = "Don't call Alloc() if the cache is a-priory incapable of allocating chunk of specified size";
		AI_FailWith("AreaAndPortalCacheAllocatorBin::Alloc()", "%s", message );
	}
#endif

	constexpr auto TAG_SIZE = 8;
	static_assert( !( TAG_SIZE % 8 ), "TAG_SIZE must be a multiple of 8" );

	// Once the freelist pool for many chunks has been initialized, it handles all allocation requests.
	if( freelistPool ) {
		// Allocate 8 extra bytes for the tag
		if( freelistPool->Size() != freelistPool->Capacity() ) {
			return SetSelfAsTag( freelistPool->Alloc( size + TAG_SIZE ) );
		} else {
			// The pool capacity has been exhausted. Fall back to using G_Malloc()
			// This is not a desired behavior but we should not crash in these extreme cases.
			return SetSelfAsTag( G_Malloc( (size_t)( size + TAG_SIZE ) ) );
		}
	}

	// Check if there is a free unpooled block saved for further allocations.
	// Check then whether no unpooled blocks were used at all, and allocate a new one.
	if( freeSingleBlock ) {
		assert( !usedSingleBlock );
		usedSingleBlock = freeSingleBlock;
		freeSingleBlock = nullptr;
		return SetSelfAsTag( usedSingleBlock );
	} else if( !usedSingleBlock ) {
		// Allocate 8 extra bytes for the tag
		usedSingleBlock = G_Malloc( (size_t)( size + TAG_SIZE ) );
		return SetSelfAsTag( usedSingleBlock );
	}

	// Too many blocks is going to be used.
	// Allocate a freelist pool in a single continuous memory block, and use it for further allocations.

	// The block size must be aligned itself, that is a FreelistPool() contract
	size_t alignedBlockSize = blockSize;
	if( alignedBlockSize % 8 ) {
		alignedBlockSize += 8 - blockSize % 8;
	}

	// Each block needs some space for the header and 8 extra bytes for the block tag
	size_t bufferSize = ( alignedBlockSize + sizeof( FreelistPool::BlockHeader ) + TAG_SIZE ) * numBlocks;
	size_t bufferAlignmentBytes = 0;
	if( sizeof( FreelistPool ) % 8 ) {
		bufferAlignmentBytes = 8 - sizeof( FreelistPool ) % 8;
	}

	size_t memSize = sizeof( FreelistPool ) + bufferAlignmentBytes + bufferSize;
	uint8_t *memBlock = (uint8_t *)G_Malloc( memSize );
	memset( memBlock, 0, memSize );
	uint8_t *poolBuffer = memBlock + sizeof( FreelistPool ) + bufferAlignmentBytes;
	// Note: It is important to tell the pool about the extra space occupied by block tags
	freelistPool = new( memBlock )FreelistPool( poolBuffer, bufferSize, alignedBlockSize + TAG_SIZE );
	return SetSelfAsTag( freelistPool->Alloc( size + TAG_SIZE ) );
}

void AreaAndPortalCacheAllocatorBin::Free( void *ptr ) {
	if( ptr != usedSingleBlock ) {
		// Check whether it has been allocated by the freelist pool
		// or has been allocated via G_Malloc() if the pool capacity has been exceeded.
		if( freelistPool->MayOwn( ptr ) ) {
			freelistPool->Free( ptr );
		} else {
			G_Free( ptr );
		}
		return;
	}

	assert( !freeSingleBlock );
	// If the freelist pool is not allocated yet at the moment of this Free() call,
	// its likely there is no further necessity in the pool.
	// Save the single block for further use.
	if( !freelistPool ) {
		freeSingleBlock = usedSingleBlock;
	} else {
		// Free the single block, as further allocations are handled by the freelist pool.
		G_Free( usedSingleBlock );
	}

	usedSingleBlock = nullptr;
}

void AiAasRouteCache::ResultCache::Clear() {
	nodes[0].prev[Node::LIST_LINKS] = NULL_LINK;
	nodes[0].next[Node::LIST_LINKS] = 1;

	for( unsigned i = 1; i < MAX_CACHED_RESULTS - 1; ++i ) {
		nodes[i].prev[Node::LIST_LINKS] = (int16_t)( i - 1 );
		nodes[i].next[Node::LIST_LINKS] = (int16_t)( i + 1 );
	}

	nodes[MAX_CACHED_RESULTS - 1].prev[Node::LIST_LINKS] = MAX_CACHED_RESULTS - 2;
	nodes[MAX_CACHED_RESULTS - 1].next[Node::LIST_LINKS] = NULL_LINK;

	freeNode = 0;
	newestUsedNode = NULL_LINK;
	oldestUsedNode = NULL_LINK;

	std::fill_n( bins, NUM_HASH_BINS, NULL_LINK );
}

inline void AiAasRouteCache::ResultCache::LinkToHashBin( uint16_t binIndex, Node *node ) {
	node->binIndex = binIndex;
	Link( node, &bins[binIndex], Node::BIN_LINKS, nodes );
}

inline void AiAasRouteCache::ResultCache::LinkToUsedList( Node *node ) {
	Link( node, &newestUsedNode, Node::LIST_LINKS, nodes );

	// If there is no oldestUsedNode, set the node as it.
	if( oldestUsedNode < 0 ) {
		oldestUsedNode = LinkOf( node );
	}
}

inline AiAasRouteCache::ResultCache::Node *AiAasRouteCache::ResultCache::UnlinkOldestUsedNode() {
	Node *result = nodes + oldestUsedNode;
	// Unlink the node from its bin
	Unlink( result, &bins[result->binIndex], Node::BIN_LINKS, nodes );
	// Unlink the node from nodes list
	Unlink( result, &oldestUsedNode, Node::LIST_LINKS, nodes );
	return result;
}

const AiAasRouteCache::ResultCache::Node *
AiAasRouteCache::ResultCache::GetCachedResultForKey( uint16_t binIndex, uint64_t key ) const {
	int16_t nodeIndex = bins[binIndex];
	while( nodeIndex >= 0 ) {
		const Node *node = nodes + nodeIndex;
		if( node->key == key ) {
			return node;
		}
		nodeIndex = node->next[Node::BIN_LINKS];
	}

	return nullptr;
}

AiAasRouteCache::ResultCache::Node *
AiAasRouteCache::ResultCache::AllocAndRegisterForKey( uint16_t binIndex, uint64_t key ) {
	Node *result;
	if( freeNode >= 0 ) {
		// Unlink the node from free list
		result = nodes + freeNode;
		Unlink( result, &freeNode, Node::LIST_LINKS, nodes );
	} else {
		result = UnlinkOldestUsedNode();
	}

	LinkToHashBin( binIndex, result );
	LinkToUsedList( result );

	result->key = key;
	return result;
}

void *AiAasRouteCache::GetClearedMemory( size_t size ) {
	void *mem = G_Malloc( size );
	::memset( mem, 0, size );
	return mem;
}

void AiAasRouteCache::FreeMemory( void *ptr ) {
	G_Free( ptr );
}

void *AiAasRouteCache::AllocAreaAndPortalCacheMemory( size_t size ) {
	// Check whether the corresponding bin chunk size is small enough
	// to allow the bin to be addressed directly by size.
	if( size < sizeof( areaAndPortalSmallBinsTable ) / sizeof( *areaAndPortalSmallBinsTable ) ) {
		if( auto *bin = areaAndPortalSmallBinsTable[size] ) {
			assert( bin->FitsSize( size ) );
			if( bin->NeedsCleanup() ) {
				FreeOldestCache();
			}
			return bin->Alloc( size );
		}

		// Create a new bin for the size
		void *mem = G_Malloc( sizeof( AreaAndPortalCacheAllocatorBin ) );
		memset( mem, 0, sizeof( AreaAndPortalCacheAllocatorBin ) );
		auto *newBin = new( mem )AreaAndPortalCacheAllocatorBin( size );
		areaAndPortalSmallBinsTable[size] = newBin;
		return newBin->Alloc( size );
	}

	// Check whether there are bins able to handle the request in the common bins list
	for( AreaAndPortalCacheAllocatorBin *bin = areaAndPortalCacheHead; bin; bin = bin->next ) {
		if( bin->FitsSize( size ) ) {
			if( bin->NeedsCleanup() ) {
				FreeOldestCache();
			}
			return bin->Alloc( size );
		}
	}

	// Create a new bin for the size
	void *mem = G_Malloc( sizeof( AreaAndPortalCacheAllocatorBin ) );
	memset( mem, 0, sizeof( AreaAndPortalCacheAllocatorBin ) );
	auto *newBin = new( mem )AreaAndPortalCacheAllocatorBin( size );

	// Link it to the bins list head
	newBin->next = areaAndPortalCacheHead;
	areaAndPortalCacheHead = newBin;

	return newBin->Alloc( size );
}

void AiAasRouteCache::FreeAreaAndPortalCacheMemory( void *ptr ) {
	// The chunk stores its owner as a tag
	AreaAndPortalCacheAllocatorBin::FreeTaggedBlock( ptr );
}

void AiAasRouteCache::FreeAreaAndPortalMemoryPools() {
	auto *bin = areaAndPortalCacheHead;
	while( bin ) {
		// Don't trigger "use after free"
		auto *nextBin = bin->next;
		G_Free( bin );
		bin = nextBin;
	}

	int binsTableCapacity = sizeof( areaAndPortalSmallBinsTable ) / sizeof( areaAndPortalSmallBinsTable[0] );
	for( int i = 0; i < binsTableCapacity; ++i ) {
		if( areaAndPortalSmallBinsTable[i] ) {
			G_Free( areaAndPortalSmallBinsTable[i] );
		}
	}
}

bool AiAasRouteCache::FreeOldestCache() {
	const auto *aasAreaSettings = aasWorld.AreaSettings();
	const auto *aasPortals = aasWorld.Portals();
	if( auto *cache = oldestCache ) {
		if( cache->prev ) {
			cache->prev->next = cache->next;
		} else {
			// TODO: Area and portal caches must belong to different lists! Avoid this branching!
			if( cache->type == CACHETYPE_AREA ) {
				auto clusterAreaNum = ClusterAreaNum( aasAreaSettings, aasPortals, cache->cluster, cache->areaNum );
				clusterAreaCache[cache->cluster][clusterAreaNum] = cache->next;
			} else {
				portalCache[cache->areaNum] = cache->next;
			}
		}
		if( cache->next ) {
			cache->next->prev = cache->prev;
		}

		FreeRoutingCache( cache );
		return true;
	}

	return false;
}

AiAasRouteCache::AreaOrPortalCacheTable *AiAasRouteCache::AllocRoutingCache( int numTravelTimes ) {
	size_t size = sizeof( AreaOrPortalCacheTable );
	size += numTravelTimes * sizeof( uint16_t );
	size += numTravelTimes * sizeof( uint8_t );

	auto *const cache = (AreaOrPortalCacheTable *)AllocAreaAndPortalCacheMemory( size );

	// TODO: We can reduce overhead of this by clearing only needed fields.
	// That's why it is not handled at AllocAreaAndPortalCacheMemory() level.
	// Doing this is mandatory and lack of clearing the memory was a source of the bug
	// that has been plaguing all attempts of using SetDisabledZones() during 2 years.
	memset( cache, 0, size );

	// The cache has a variable size.
	// AreaOrPortalCacheTable acts as its header.
	// A variable-sized travel times array follows.
	// Then a variable-sized reachabilities array is put.
	cache->reachabilities = (uint8_t *)cache + sizeof( AreaOrPortalCacheTable ) + numTravelTimes * sizeof( uint16_t );
	cache->size = (int)size;
	return cache;
}

void AiAasRouteCache::FreeAllClusterAreaCache() {
	if( !clusterAreaCache ) {
		return;
	}

	ResetAllClusterAreaCache();
	FreeMemory( clusterAreaCache );
	clusterAreaCache = nullptr;
}

void AiAasRouteCache::ResetAllClusterAreaCache() {
	assert( clusterAreaCache );

	const auto *const aasClusters = aasWorld.Clusters();
	for( int i = 0, end = aasWorld.NumClusters(); i < end; i++ ) {
		const auto *cluster = &aasClusters[i];
		for( int j = 0; j < cluster->numareas; j++ ) {
			AreaOrPortalCacheTable *nextCache;
			for( auto *cache = clusterAreaCache[i][j]; cache; cache = nextCache ) {
				nextCache = cache->next;
				FreeAreaAndPortalCacheMemory( cache );
			}
			clusterAreaCache[i][j] = nullptr;
		}
	}
}

void AiAasRouteCache::InitClusterAreaCache() {
	const auto *const aasClusters = aasWorld.Clusters();
	const auto numClusters = aasWorld.NumClusters();

	size_t totalNumAreas = 0;
	for( int i = 0; i < numClusters; i++ ) {
		totalNumAreas += aasClusters[i].numareas;
	}

	// The "cluster are cache" is a two dimensional array with pointers
	// for every cluster to routing cache for every area in that cluster
	size_t numBytes = numClusters * sizeof( AreaOrPortalCacheTable ** );
	numBytes += totalNumAreas * sizeof( AreaOrPortalCacheTable * );
	auto *ptr = (uint8_t *) GetClearedMemory( numBytes );

	clusterAreaCache = (AreaOrPortalCacheTable ***)ptr;

	ptr += numClusters * sizeof( AreaOrPortalCacheTable ** );
	for( int i = 0; i < numClusters; i++ ) {
		clusterAreaCache[i] = CastCheckingAlignment<AreaOrPortalCacheTable *>( ptr );
		ptr += aasClusters[i].numareas * sizeof( AreaOrPortalCacheTable * );
	}
}

void AiAasRouteCache::FreeAllPortalCache() {
	if( !portalCache ) {
		return;
	}

	ResetAllPortalCache();
	FreeMemory( portalCache );
	portalCache = nullptr;
}

void AiAasRouteCache::ResetAllPortalCache() {
	assert( portalCache );
	for( int i = 0, end = aasWorld.NumAreas(); i < end; i++ ) {
		AreaOrPortalCacheTable *nextCache;
		for( auto *cache = portalCache[i]; cache; cache = nextCache ) {
			nextCache = cache->next;
			FreeAreaAndPortalCacheMemory( cache );
		}
		portalCache[i] = nullptr;
	}
}

void AiAasRouteCache::InitPortalCache() {
	portalCache = (AreaOrPortalCacheTable **)GetClearedMemory( aasWorld.NumAreas() * sizeof( AreaOrPortalCacheTable * ) );
}

void AiAasRouteCache::InitPathFindingNodes() {
	const auto *aasClusters = aasWorld.Clusters();

	maxReachAreas = 0;
	for( int i = 0, end = aasWorld.NumClusters(); i < end; i++ ) {
		int numReachAreas = aasClusters[i].numreachabilityareas;
		if( numReachAreas > maxReachAreas ) {
			maxReachAreas = numReachAreas;
		}
	}

	areaPathFindingNodes = (PathFinderNode *)GetClearedMemory( maxReachAreas * sizeof( PathFinderNode ) );
	portalPathFindingNodes = (PathFinderNode *)GetClearedMemory( ( aasWorld.NumPortals() + 1 ) * sizeof( PathFinderNode ) );

	oldestCache = nullptr;
	newestCache = nullptr;
}

/**
 * We force 4-byte alignment in hope a compiler will operate with this data type
 * as with a single 32-bit word and not as two smaller values
 */
struct alignas( 4 )RoutingUpdateRef {
	uint16_t index;
	uint16_t tmpTravelTime;

	RoutingUpdateRef( int index_, uint16_t tmpTravelTime_ )
		: index( ToUint16CheckingRange( index_ ) ), tmpTravelTime( tmpTravelTime_ ) {}

	bool operator<( const RoutingUpdateRef &that ) const {
		return tmpTravelTime > that.tmpTravelTime;
	}
};

static_assert( sizeof( RoutingUpdateRef ) == 4, "" );
static_assert( alignof( RoutingUpdateRef ) == 4, "" );

// Dijkstra's algorithm labels
constexpr const int8_t UNREACHED = 0;
constexpr const int8_t LABELED = -1;
constexpr const int8_t SCANNED = +1;

void AiAasRouteCache::UpdateAreaRoutingCache( const aas_areasettings_t *aasAreaSettings,
											  const aas_portal_t *aasPortals,
											  AreaOrPortalCacheTable *areaCache ) {
	//NOTE: not more than 128 reachabilities per area allowed
	uint16_t startAreaTravelTimes[128];

	//number of reachability areas within this cluster
	const int numReachAreas = aasWorld.Clusters()[areaCache->cluster].numreachabilityareas;
	const int badTravelFlags = ~areaCache->travelFlags;

	auto clusterAreaNum = ClusterAreaNum( aasAreaSettings, aasPortals, areaCache->cluster, areaCache->areaNum );
	if( clusterAreaNum >= numReachAreas ) {
		return;
	}

	memset( startAreaTravelTimes, 0, sizeof( startAreaTravelTimes ) );

	// Precache all references to avoid pointer chasing in loop
	const auto *const aasRevReach = this->aasRevReach;
	const auto *const aasRevLinks = this->aasRevLinks;
	auto *const pathFindingNodes = this->areaPathFindingNodes;
	const auto *const areaPathFindingData = this->areaPathFindingData;
	const auto *const reachPathFindingData = this->reachPathFindingData;

	for( int i = 0, end = maxReachAreas; i < end; ++i ) {
		pathFindingNodes[i].dijkstraLabel = UNREACHED;
	}

	PathFinderNode *currNode = &pathFindingNodes[clusterAreaNum];
	currNode->areaNum = areaCache->areaNum;
	currNode->areaTravelTimes = startAreaTravelTimes;
	currNode->tmpTravelTime = ToUint16CheckingRange( areaCache->startTravelTime );
	areaCache->travelTimes[clusterAreaNum] = ToUint16CheckingRange( areaCache->startTravelTime );
	currNode->dijkstraLabel = LABELED;

	StaticVector<RoutingUpdateRef, 1024> updateHeap;
	updateHeap.push_back( RoutingUpdateRef( clusterAreaNum, currNode->tmpTravelTime ) );

	//while there are updates in the current list
	while( !updateHeap.empty() ) {
		std::pop_heap( updateHeap.begin(), updateHeap.end() );
		RoutingUpdateRef currUpdateRef = updateHeap.back();
		currNode = &pathFindingNodes[currUpdateRef.index];
		currNode->dijkstraLabel = SCANNED;
		updateHeap.pop_back();

		// Check all reversed reachability links
		const auto &revReach = aasRevReach[currNode->areaNum];
		const int numLinks = revReach.numLinks;
		int revLinkNum = revReach.firstRevLink;
		const RevLink *revLink;
		for( int revLinkAreaIndex = 0; revLinkAreaIndex < numLinks; revLinkNum = revLink->nextLink, revLinkAreaIndex++ ) {
			revLink = aasRevLinks + revLinkNum;
			const auto reachNum = revLink->linkNum;
			const auto &reachData = reachPathFindingData[reachNum];
			// If the reachability has an undesired travel type
			if( reachData.travelFlags & badTravelFlags ) {
				continue;
			}
			// Number of the area the reversed reachability leads to
			const auto nextAreaNum = revLink->areaNum;

			const auto &nextAreaData = areaPathFindingData[nextAreaNum];
			// If it is not allowed to enter the next area
			if( nextAreaData.disabledStatus.CurrStatus() ) {
				continue;
			}
			// Respect global flags too
			if( nextAreaData.settingsAreaFlags & AREA_DISABLED ) {
				continue;
			}
			// If the next area has a not allowed travel flag
			if( nextAreaData.contentsTravelFlags & badTravelFlags ) {
				continue;
			}

			// Get the cluster number of the area
			const auto clusterOrPortalNum = nextAreaData.clusterOrPortalNum;
			// Don't leave the cluster
			if( clusterOrPortalNum > 0 && clusterOrPortalNum != areaCache->cluster ) {
				continue;
			}

			clusterAreaNum = nextAreaData.clusterAreaNum;
			if( clusterAreaNum >= numReachAreas ) {
				continue;
			}

			auto *const nextNode = &pathFindingNodes[clusterAreaNum];
			if( nextNode->dijkstraLabel != UNREACHED ) {
				continue;
			}

			// Time already travelled
			// plus the traveltime through the current area
			// plus the travel time from the reachability
			int relaxedTime = currNode->tmpTravelTime;
			relaxedTime += currNode->areaTravelTimes[revLinkAreaIndex];
			relaxedTime += reachData.travelTime;
			// We must check overflow, should never happen in production
			uint16_t t = ToUint16CheckingRange( relaxedTime );
			// Try to avoid ledge areas to prevent unintended falling
			// by increasing the travel time by some penalty value (3 seconds).
			// That's an idea from Doom 3 source code.
			// Apply penalty on areas that do not look like useful as well
			const auto nextAreaFlags = nextAreaData.settingsAreaFlags;
			if( nextAreaFlags & ( AREA_LEDGE | AREA_JUNK ) ) {
				if( nextAreaFlags & AREA_WALL ) {
					if( nextAreaFlags & AREA_LEDGE ) {
						// If this area has a wall, it usually cannot be avoided, so apply lesser penalty
						t = ToUint16CheckingRange( t + 50 );
					}
					if( nextAreaFlags & AREA_JUNK ) {
						t = ToUint16CheckingRange( t + 100 );
					}
				} else {
					if( nextAreaFlags & AREA_LEDGE ) {
						t = ToUint16CheckingRange( t + 100 );
					}
					if( nextAreaFlags & AREA_JUNK ) {
						t = ToUint16CheckingRange( t + 50 );
					}
				}
			}

			// Check whether we can "relax" the edge (in Dijkstra terms)
			auto *const timeToRelax = &areaCache->travelTimes[clusterAreaNum];
			if( *timeToRelax && *timeToRelax <= t ) {
				continue;
			}

			*timeToRelax = t;

			const auto reachOffset = reachNum - nextAreaData.firstReachNum;
			assert( (unsigned)reachOffset < 255 );
			areaCache->reachabilities[clusterAreaNum] = (uint8_t)reachOffset;

			nextNode->areaNum = ToUint16CheckingRange( nextAreaNum );
			nextNode->tmpTravelTime = t;
			nextNode->areaTravelTimes = areaTravelTimes[nextAreaNum][reachOffset];
			nextNode->dijkstraLabel = LABELED;
			updateHeap.push_back( RoutingUpdateRef( clusterAreaNum, t ) );
			std::push_heap( updateHeap.begin(), updateHeap.end() );
		}
	}
}

AiAasRouteCache::AreaOrPortalCacheTable *
AiAasRouteCache::GetAreaRoutingCache( const aas_areasettings_t *aasAreaSettings,
									  const aas_portal_t *aasPortals,
									  int clusterNum, int areaNum, int travelFlags ) {
	//number of the area in the cluster
	const auto clusterAreaNum = ClusterAreaNum( aasAreaSettings, aasPortals, clusterNum, areaNum );
	//find the cache without undesired travel flags
	AreaOrPortalCacheTable *cache = clusterAreaCache[clusterNum][clusterAreaNum];
	for(; cache; cache = cache->next ) {
		//if there aren't used any undesired travel types for the cache
		if( cache->travelFlags == travelFlags ) {
			break;
		}
	}

	if( !cache ) {
		cache = AllocRoutingCache( aasWorld.Clusters()[clusterNum].numreachabilityareas );
		cache->cluster = ToUint16CheckingRange( clusterNum );
		cache->areaNum = ToUint16CheckingRange( areaNum );
		cache->startTravelTime = 1;
		cache->travelFlags = travelFlags;
		cache->prev = nullptr;
		// Warning! Do not precache this reference at the beginning!
		// AllocRoutingCache() calls might modify the member!
		auto *oldCacheHead = clusterAreaCache[clusterNum][clusterAreaNum];
		cache->next = oldCacheHead;
		if( oldCacheHead ) {
			oldCacheHead->prev = cache;
		}
		clusterAreaCache[clusterNum][clusterAreaNum] = cache;
		UpdateAreaRoutingCache( aasAreaSettings, aasPortals, cache );
	} else {
		UnlinkCache( cache );
	}

	cache->type = CACHETYPE_AREA;
	LinkCache( cache );
	return cache;
}

void AiAasRouteCache::UpdatePortalRoutingCache( const aas_areasettings_t *aasAreaSettings,
												const aas_portal_t *aasPortals,
												AreaOrPortalCacheTable *portalCache ) {
	const auto *const aasPortalIndex = aasWorld.PortalIndex();
	const auto *const aasClusters = aasWorld.Clusters();
	auto *const portalMaxTravelTimes = this->portalMaxTravelTimes;
	auto *const pathFindingNodes = this->portalPathFindingNodes;

	const auto numPortals = aasWorld.NumPortals();
	for( int i = 0; i < numPortals + 1; ++i ) {
		pathFindingNodes[i].dijkstraLabel = UNREACHED;
	}

	PathFinderNode *currNode = &pathFindingNodes[numPortals];
	currNode->cluster = portalCache->cluster;
	currNode->areaNum = portalCache->areaNum;
	currNode->tmpTravelTime = ToUint16CheckingRange( portalCache->startTravelTime );
	currNode->dijkstraLabel = LABELED;

	// If the start area is a cluster portal, store the travel time for that portal
	const auto clusterNum = aasAreaSettings[portalCache->areaNum].cluster;
	if( clusterNum < 0 ) {
		portalCache->travelTimes[-clusterNum] = ToUint16CheckingRange( portalCache->startTravelTime );
	}

	StaticVector<RoutingUpdateRef, 1024> updateHeap;
	updateHeap.push_back( RoutingUpdateRef( numPortals, ToUint16CheckingRange( portalCache->startTravelTime ) ) );

	//while there are updates in the current list
	while( !updateHeap.empty() ) {
		std::pop_heap( updateHeap.begin(), updateHeap.end() );
		currNode = &portalPathFindingNodes[updateHeap.back().index];
		currNode->dijkstraLabel = SCANNED;
		updateHeap.pop_back();

		// Fix invalid access to cluster 0
		if( !currNode->cluster ) {
			continue;
		}

		const auto *cluster = &aasClusters[currNode->cluster];
		const auto *cache = GetAreaRoutingCache( aasAreaSettings, aasPortals, currNode->cluster,
												 currNode->areaNum, portalCache->travelFlags );
		// Take all portals of the cluster
		for( int i = 0; i < cluster->numportals; i++ ) {
			const auto portalNum = aasPortalIndex[cluster->firstportal + i];
			const auto *portal = &aasPortals[portalNum];
			//if this is the portal of the current update continue
			if( portal->areanum == currNode->areaNum ) {
				continue;
			}
			auto clusterAreaNum = ClusterAreaNum( aasAreaSettings, aasPortals, currNode->cluster, portal->areanum );
			if( clusterAreaNum >= cluster->numreachabilityareas ) {
				continue;
			}

			uint16_t t = cache->travelTimes[clusterAreaNum];
			if( !t ) {
				continue;
			}
			t = ToUint16CheckingRange( t + currNode->tmpTravelTime );

			// Check whether we can "relax" the edge (in Dijkstra term)
			if( portalCache->travelTimes[portalNum] && portalCache->travelTimes[portalNum] <= t ) {
				continue;
			}

			portalCache->travelTimes[portalNum] = t;
			auto *const nextNode = &pathFindingNodes[portalNum];
			if( portal->frontcluster == currNode->cluster ) {
				nextNode->cluster = ToUint16CheckingRange( portal->backcluster );
			} else {
				nextNode->cluster = ToUint16CheckingRange( portal->frontcluster );
			}
			nextNode->areaNum = ToUint16CheckingRange( portal->areanum );
			//add travel time through the actual portal area for the next update
			nextNode->tmpTravelTime = ToUint16CheckingRange( t + portalMaxTravelTimes[portalNum] );
			if( nextNode->dijkstraLabel != UNREACHED ) {
				continue;
			}

			nextNode->dijkstraLabel = LABELED;
			updateHeap.push_back( RoutingUpdateRef( portalNum, nextNode->tmpTravelTime ) );
			std::push_heap( updateHeap.begin(), updateHeap.end() );
		}
	}
}

AiAasRouteCache::AreaOrPortalCacheTable *
AiAasRouteCache::GetPortalRoutingCache( const aas_areasettings_t *aasAreaSettings,
										const aas_portal_t *aasPortals,
										int clusterNum, int areaNum, int travelFlags ) {
	AreaOrPortalCacheTable *cache;
	//find the cached portal routing if existing
	for( cache = portalCache[areaNum]; cache; cache = cache->next ) {
		if( cache->travelFlags == travelFlags ) {
			break;
		}
	}
	//if the portal routing isn't cached
	if( !cache ) {
		cache = AllocRoutingCache( aasWorld.NumPortals() );
		cache->cluster = ToUint16CheckingRange( clusterNum );
		cache->areaNum = ToUint16CheckingRange( areaNum );
		cache->startTravelTime = 1;
		cache->travelFlags = travelFlags;
		//add the cache to the cache list
		cache->prev = nullptr;
		// Warning! Do not precache this reference at the beginning!
		// AllocRoutingCache() calls might modify the member!
		auto *oldCacheHead = portalCache[areaNum];
		cache->next = oldCacheHead;
		if( oldCacheHead ) {
			oldCacheHead->prev = cache;
		}
		portalCache[areaNum] = cache;
		//update the cache
		UpdatePortalRoutingCache( aasAreaSettings, aasPortals, cache );
	} else {
		UnlinkCache( cache );
	}
	//the cache has been accessed
	cache->type = CACHETYPE_PORTAL;
	LinkCache( cache );
	return cache;
}

int AiAasRouteCache::PreferredRouteToGoalArea( int fromAreaNum, int toAreaNum, int *reachNum ) const {
	for( int i = 0; i < 2; ++i ) {
		RoutingResult routingResult;
		if( RoutingResultToGoalArea( fromAreaNum, toAreaNum, travelFlags[i], &routingResult ) ) {
			*reachNum = routingResult.reachNum;
			return routingResult.travelTime;
		}
	}

	return 0;
}

int AiAasRouteCache::PreferredRouteToGoalArea( const int *fromAreaNums, int numFromAreas, int toAreaNum, int *reachNum ) const {
	for( int i = 0; i < 2; ++i ) {
		for( int j = 0; j < numFromAreas; ++j ) {
			RoutingResult routingResult;
			if( RoutingResultToGoalArea( fromAreaNums[j], toAreaNum, travelFlags[i], &routingResult ) ) {
				*reachNum = routingResult.reachNum;
				return routingResult.travelTime;
			}
		}
	}

	return 0;
}

int AiAasRouteCache::FastestRouteToGoalArea( int fromAreaNum, int toAreaNum, int *reachNum ) const {
	int bestTravelTime = std::numeric_limits<int>::max();
	int bestReachNum = 0;

	for( int i = 0; i < 2; ++i ) {
		RoutingResult routingResult;
		if( RoutingResultToGoalArea( fromAreaNum, toAreaNum, travelFlags[i], &routingResult ) ) {
			if( bestTravelTime > routingResult.travelTime ) {
				bestTravelTime = routingResult.travelTime;
				bestReachNum = routingResult.reachNum;
			}
		}
	}

	if( bestTravelTime == std::numeric_limits<int>::max() ) {
		return 0;
	}

	*reachNum = bestReachNum;
	return bestTravelTime;
}

int AiAasRouteCache::FastestRouteToGoalArea( const int *fromAreaNums, int numFromAreas,
											 int toAreaNum, int *reachNum ) const {
	int bestTravelTime = std::numeric_limits<int>::max();
	int bestReachNum = 0;

	for( int i = 0; i < 2; ++i ) {
		for( int j = 0; j < numFromAreas; ++j ) {
			RoutingResult routingResult;
			if( RoutingResultToGoalArea( fromAreaNums[j], toAreaNum, travelFlags[i], &routingResult ) ) {
				if( bestTravelTime > routingResult.travelTime ) {
					bestTravelTime = routingResult.travelTime;
					bestReachNum = routingResult.reachNum;
				}
			}
		}
	}

	if( bestTravelTime == std::numeric_limits<int>::max() ) {
		return 0;
	}

	*reachNum = bestReachNum;
	return bestTravelTime;
}

bool AiAasRouteCache::RoutingResultToGoalArea( int fromAreaNum, int toAreaNum,
											   int travelFlags, RoutingResult *result ) const {
	if( fromAreaNum == toAreaNum ) {
		result->travelTime = 1;
		result->reachNum = 0;
		return true;
	}

	if( fromAreaNum <= 0 || fromAreaNum >= aasWorld.NumAreas() ) {
		return false;
	}

	if( toAreaNum <= 0 || toAreaNum >= aasWorld.NumAreas() ) {
		return false;
	}

	if( aasWorld.AreaDoNotEnter( fromAreaNum ) || aasWorld.AreaDoNotEnter( toAreaNum ) ) {
		travelFlags |= TFL_DONOTENTER;
	}

	auto *nonConstThis = const_cast<AiAasRouteCache *>( this );

	const uint64_t key = ResultCache::Key( fromAreaNum, toAreaNum, travelFlags );
	const uint16_t binIndex = ResultCache::BinIndexForKey( key );
	if( auto *cacheNode = resultCache.GetCachedResultForKey( binIndex, key ) ) {
		result->reachNum = cacheNode->reachability;
		result->travelTime = cacheNode->travelTime;
		return cacheNode->reachability != 0;
	}

	auto *cacheNode = nonConstThis->resultCache.AllocAndRegisterForKey( binIndex, key );
	RoutingRequest request( fromAreaNum, toAreaNum, travelFlags );
	if( nonConstThis->RouteToGoalArea( request, result ) ) {
		cacheNode->reachability = ToUint16CheckingRange( result->reachNum );
		cacheNode->travelTime = ToUint16CheckingRange( result->travelTime );
		return true;
	}

	cacheNode->reachability = 0;
	cacheNode->travelTime = 0;
	return false;
}

bool AiAasRouteCache::RouteToGoalArea( const RoutingRequest &request, RoutingResult *result ) {
	const auto *const aasAreaSettings = aasWorld.AreaSettings();
	const auto *const aasPortals = aasWorld.Portals();

	auto clusterNum = aasAreaSettings[request.areaNum].cluster;
	auto goalClusterNum = aasAreaSettings[request.goalAreaNum].cluster;
	// Check if the area is a portal of the goal area cluster
	if( clusterNum < 0 && goalClusterNum > 0 ) {
		const auto *portal = &aasPortals[-clusterNum];
		if( portal->frontcluster == goalClusterNum || portal->backcluster == goalClusterNum ) {
			clusterNum = goalClusterNum;
		}
	}
	// Check if the goalarea is a portal of the area cluster
	else if( clusterNum > 0 && goalClusterNum < 0 ) {
		const aas_portal_t *portal = &aasPortals[-goalClusterNum];
		if( portal->frontcluster == clusterNum || portal->backcluster == clusterNum ) {
			goalClusterNum = clusterNum;
		}
	}
	// Fix invalid access to cluster 0
	else if( !clusterNum || !goalClusterNum ) {
		return false;
	}

	// If both areas are in the same cluster
	// NOTE: there might be a shorter route via another cluster!!! but we don't care
	if( clusterNum > 0 && goalClusterNum > 0 && clusterNum == goalClusterNum ) {
		const auto *areaCache = GetAreaRoutingCache( aasAreaSettings, aasPortals, clusterNum,
													 request.goalAreaNum, request.travelFlags );
		// The number of the area in the cluster
		const auto clusterAreaNum = ClusterAreaNum( aasAreaSettings, aasPortals, clusterNum, request.areaNum );
		// The cluster the area is in
		const auto *cluster = &aasWorld.Clusters()[clusterNum];
		// If the area is NOT a reachability area
		if( clusterAreaNum >= cluster->numreachabilityareas ) {
			return false;
		}
		// If it is possible to travel to the goal area through this cluster
		if( areaCache->travelTimes[clusterAreaNum] != 0 ) {
			result->reachNum = aasAreaSettings[request.areaNum].firstreachablearea;
			result->reachNum += areaCache->reachabilities[clusterAreaNum];
			result->travelTime = areaCache->travelTimes[clusterAreaNum];
			return true;
		}
	}

	goalClusterNum = aasAreaSettings[request.goalAreaNum].cluster;
	// If the goal area is a portal
	if( goalClusterNum < 0 ) {
		// Just assume the goal area is part of the front cluster
		goalClusterNum = aasPortals[-goalClusterNum].frontcluster;
	}

	auto *portalCache = GetPortalRoutingCache( aasAreaSettings, aasPortals, goalClusterNum,
											   request.goalAreaNum, request.travelFlags );
	return RouteToGoalPortal( request, portalCache, result );
}

bool AiAasRouteCache::RouteToGoalPortal( const RoutingRequest &request,
										 AreaOrPortalCacheTable *portalCache,
										 RoutingResult *result ) {
	const auto *const aasAreaSettings = aasWorld.AreaSettings();
	const auto clusterNum = aasAreaSettings[request.areaNum].cluster;
	// If the area is a cluster portal, read directly from the portal cache
	if( clusterNum < 0 ) {
		result->travelTime = portalCache->travelTimes[-clusterNum];
		result->reachNum = aasAreaSettings[request.areaNum].firstreachablearea;
		result->reachNum += portalCache->reachabilities[-clusterNum];
		return true;
	}

	const auto *const aasPortalIndex = aasWorld.PortalIndex();
	const auto *const aasPortals = aasWorld.Portals();
	// The cluster the area is in
	const auto *cluster = &aasWorld.Clusters()[clusterNum];

	int bestTime = 0;
	int bestReachNum = -1;
	// Find the portal of the area cluster leading towards the goal area
	for( int i = 0; i < cluster->numportals; i++ ) {
		const auto portalNum = aasPortalIndex[cluster->firstportal + i];
		// If the goal area isn't reachable from the portal
		const auto travelTimeFromPortalToGoal = portalCache->travelTimes[portalNum];
		if( !travelTimeFromPortalToGoal ) {
			continue;
		}

		const auto *portal = &aasPortals[portalNum];
		// Get the cache of the portal area
		const auto *areaCache = GetAreaRoutingCache( aasAreaSettings, aasPortals, clusterNum,
													 portal->areanum, request.travelFlags );
		// Current area inside the current cluster
		const auto clusterAreaNum = ClusterAreaNum( aasAreaSettings, aasPortals, clusterNum, request.areaNum );
		// If the area is NOT a reachability area
		if( clusterAreaNum >= cluster->numreachabilityareas ) {
			continue;
		}
		// If the portal is NOT reachable from this area
		const auto areaToPortalTravelTime = areaCache->travelTimes[clusterAreaNum];
		if( !areaToPortalTravelTime ) {
			continue;
		}

		// Total travel time is the travel time the portal area is from
		// the goal area plus the travel time towards the portal area
		uint16_t t = ToUint16CheckingRange( travelTimeFromPortalToGoal + areaToPortalTravelTime );
		//FIXME: add the exact travel time through the actual portal area
		//NOTE: for now we just add the largest travel time through the portal area
		//		because we can't directly calculate the exact travel time
		//		to be more specific we don't know which reachability was used to travel
		//		into the portal area
		t = ToUint16CheckingRange( t + portalMaxTravelTimes[portalNum] );
		// Check whether the time is better than the one already found
		if( bestTime && t >= bestTime ) {
			continue;
		}

		auto reachNum = aasAreaSettings[request.areaNum].firstreachablearea + areaCache->reachabilities[clusterAreaNum];
		bestReachNum = reachNum;
		bestTime = t;
	}

	if( bestReachNum < 0 ) {
		return false;
	}

	result->reachNum = bestReachNum;
	result->travelTime = bestTime;
	return true;
}
