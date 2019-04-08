#ifndef QFUSION_TACTICAL_SPOTS_DETECTOR_H
#define QFUSION_TACTICAL_SPOTS_DETECTOR_H

#include "../ai_local.h"
#include "../navigation/AasRouteCache.h"
#include "../static_vector.h"
#include "../bot.h"

class TacticalSpotsRegistry
{
	friend class BotRoamingManager;
	friend class TacticalSpotsBuilder;
	friend class TacticalSpotsProblemSolver;
	friend class AdvantageProblemSolver;
	friend class CoverProblemSolver;
	friend class DodgeHazardProblemSolver;
	friend class SideStepDodgeProblemSolver;
public:
	class OriginParams {
		friend class TacticalSpotsRegistry;
		friend class TacticalSpotsProblemSolver;
		friend class AdvantageProblemSolver;
		friend class CoverProblemSolver;
		friend class DodgeHazardProblemSolver;
		friend class SideStepDodgeProblemSolver;

		const edict_t *originEntity;
		vec3_t origin;
		float searchRadius;
		const AiAasRouteCache *routeCache;
		int originAreaNum;
	public:
		OriginParams( const edict_t *originEntity_, float searchRadius_, const AiAasRouteCache *routeCache_ )
			: originEntity( originEntity_ ), searchRadius( searchRadius_ ), routeCache( routeCache_ ) {
			VectorCopy( originEntity_->s.origin, this->origin );
			const AiAasWorld *aasWorld = AiAasWorld::Instance();
			originAreaNum = aasWorld->IsLoaded() ? aasWorld->FindAreaNum( originEntity ) : 0;
		}

		OriginParams( const vec3_t origin_, float searchRadius_, const AiAasRouteCache *routeCache_ )
			: originEntity( nullptr ), searchRadius( searchRadius_ ), routeCache( routeCache_ ) {
			VectorCopy( origin_, this->origin );
			const AiAasWorld *aasWorld = AiAasWorld::Instance();
			originAreaNum = aasWorld->IsLoaded() ? aasWorld->FindAreaNum( origin ) : 0;
		}

		OriginParams( const vec3_t origin_, const edict_t *originEntity_,
					  float searchRadius_, const AiAasRouteCache *routeCache_ )
			: originEntity( originEntity_ ), searchRadius( searchRadius_ ), routeCache( routeCache_ ) {
			VectorCopy( origin_, this->origin );
			const AiAasWorld *aasWorld = AiAasWorld::Instance();
			originAreaNum = aasWorld->IsLoaded() ? aasWorld->FindAreaNum( originEntity ) : 0;
		}

		inline Vec3 MinBBoxBounds( float minHeightAdvantage = 0.0f ) const {
			return Vec3( -searchRadius, -searchRadius, minHeightAdvantage ) + origin;
		}

		inline Vec3 MaxBBoxBounds() const {
			return Vec3( +searchRadius, +searchRadius, +searchRadius ) + origin;
		}
	};

	struct TacticalSpot {
		vec3_t origin;
		vec3_t absMins;
		vec3_t absMaxs;
		int aasAreaNum;
	};

public:
	// Make sure we can also use MAX_SPOTS + 1 to indicate illegal spot
	static constexpr uint16_t MAX_SPOTS = std::numeric_limits<uint16_t>::max() - 1;

	typedef StaticVector<uint16_t, MAX_SPOTS> SpotsQueryVector;

	struct alignas( 2 )SpotAndScore {
		FloatAlign2 score;
		uint16_t spotNum;

		SpotAndScore( uint16_t spotNum_, float score_ ) : score( score_ ), spotNum( spotNum_ ) {}
		bool operator<( const SpotAndScore &that ) const { return score > that.score; }
	};

	typedef StaticVector<SpotAndScore, MAX_SPOTS> SpotsAndScoreVector;
private:
	class TemporariesAllocator {
		// These values should be allocated, cached and used as buffers for spots query/problem params solving.
		// TODO: Check alignment for StaticVector?
		SpotsQueryVector *query { new( G_Malloc( sizeof( SpotsQueryVector ) ) )SpotsQueryVector };
		bool *excludedSpotsMask { (bool *)G_Malloc( sizeof( bool ) * MAX_SPOTS ) };

		struct SpotsAndScoreCacheEntry {
			SpotsAndScoreCacheEntry *next { nullptr };
			SpotsAndScoreCacheEntry *prev { nullptr };
			SpotsAndScoreVector data;
		};

		SpotsAndScoreCacheEntry *freeHead { nullptr };
		SpotsAndScoreCacheEntry *usedHead { nullptr };
		SpotsAndScoreCacheEntry *usedTail { nullptr };
	public:
		// TODO: We do not require explicit releasing of query vector, this is error-prone...
		SpotsQueryVector &GetCleanQueryVector() {
			query->clear();
			return *query;
		}

		bool *GetCleanExcludedSpotsMask() {
			memset( excludedSpotsMask, 0, sizeof( bool ) * MAX_SPOTS );
			return excludedSpotsMask;
		}

		SpotsAndScoreVector &GetNextCleanSpotsAndScoreVector();

		void Release();

		~TemporariesAllocator();
	} temporariesAllocator;

	static constexpr uint16_t MAX_SPOTS_PER_QUERY = 768;
	static constexpr uint16_t MIN_GRID_CELL_SIDE = 512;
	static constexpr uint16_t MAX_GRID_DIMENSION = 32;

	// i-th element contains a spot for i=spotNum
	TacticalSpot *spots { nullptr };
	// For i-th spot element # i * numSpots + j contains a mutual visibility between spots i-th and j-th spot:
	// 0 if spot origins and bounds are completely invisible for each other
	// ...
	// 255 if spot origins and bounds are completely visible for each other
	uint8_t *spotVisibilityTable { nullptr };
	// Contains a 2-dimensional array of travel time pairs ("from spot to area", "from area to spot").
	// An every cell has two values and the total number of short elements is 2 * numAreas * numSpots.
	// An outer index corresponds to an area number.
	// This is for CPU cache utilization efficiency (usually many spots are tested against the same area).
	// Travel times are computed using a shared AAS route cache and Bot::ALLOWED_TRAVEL_FLAGS.
	// Thus a path might not exist for a particular bot as individual route caches usually have additional restrictions.
	// Regardless of that values of this table are very useful for cutting off
	// non-feasible spots/areas before making expensive actual routing calls.
	uint16_t *spotsAndAreasTravelTimeTable { nullptr };

	unsigned numSpots { 0 };

	bool needsSavingPrecomputedData { false };

	class SpotsGridBuilder;

	class BaseSpotsGrid {
		friend class TacticalSpotsRegistry::SpotsGridBuilder;
	protected:
		TacticalSpotsRegistry *parent;

		TacticalSpot *spots { nullptr };
		unsigned numSpots { 0 };

		vec3_t worldMins;
		vec3_t worldMaxs;
		unsigned gridCellSize[3];
		unsigned gridNumCells[3];

		inline unsigned PointGridCellNum( const vec3_t point ) const;
		void SetupGridParams();

	public:
		explicit BaseSpotsGrid( TacticalSpotsRegistry *parent_ ): parent( parent_ ) {}

		BaseSpotsGrid( const BaseSpotsGrid &that ) = delete;
		BaseSpotsGrid &operator=( const BaseSpotsGrid &that ) = delete;
		BaseSpotsGrid( BaseSpotsGrid &&that ) = delete;
		BaseSpotsGrid &operator=( BaseSpotsGrid &&that ) = delete;

		virtual ~BaseSpotsGrid() = default;

		inline unsigned NumGridCells() const { return gridNumCells[0] * gridNumCells[1] * gridNumCells[2]; }

		const float *WorldMins() const { return &worldMins[0]; }
		const float *WorldMaxs() const { return &worldMaxs[0]; }

		void AttachSpots( TacticalSpot *spots_, unsigned numSpots_ ) {
			this->spots = spots_;
			this->numSpots = numSpots_;
		}

		virtual SpotsQueryVector &FindSpotsInRadius( const OriginParams &originParams, uint16_t *insideSpotNum ) const;

		virtual uint16_t *GetCellSpotsList( unsigned gridCellNum, uint16_t *numCellSpots ) const = 0;
	};

	class PrecomputedSpotsGrid final: public BaseSpotsGrid {
		friend class TacticalSpotsRegistry::SpotsGridBuilder;

		// i-th element contains an offset of a grid cell spot nums list for i=cellNum
		uint32_t *gridListOffsets { nullptr };
		// Contains packed lists of grid cell spot nums.
		// Each list starts by number of spot nums followed by spot nums.
		uint16_t *gridSpotsLists { nullptr };
	public:
		PrecomputedSpotsGrid( TacticalSpotsRegistry *parent_ ): BaseSpotsGrid( parent_ ) {}

		~PrecomputedSpotsGrid() override;

		bool IsLoaded() const { return gridListOffsets != nullptr; }
		bool Load( class AiPrecomputedFileReader &reader );
		void Save( class AiPrecomputedFileWriter &writer );

		SpotsQueryVector &FindSpotsInRadius( const OriginParams &originParams, uint16_t *insideSpotNum ) const override;

		uint16_t *GetCellSpotsList( unsigned gridCellNum, uint16_t *numCellSpots ) const override;
	};

	class SpotsGridBuilder final: public BaseSpotsGrid {
		// Contains a list of spot nums for the grid cell
		struct GridSpotsArray {
			uint16_t internalBuffer[8];
			uint16_t *data { internalBuffer };
			uint16_t size { 0 };
			uint16_t capacity { 8 };

			~GridSpotsArray() {
				if( data != internalBuffer ) {
					G_LevelFree( data );
				}
			}

			void AddSpot( uint16_t spotNum );
		};

		// A sparse storage for grid cell spots lists used for grid building.
		// Each array element corresponds to a grid cell, and might be null.
		// Built cells spots list get compactified while being copied to a PrecomputedSpotsGrid.
		GridSpotsArray **gridSpotsArrays { nullptr };
	public:
		explicit SpotsGridBuilder( TacticalSpotsRegistry *parent );

		~SpotsGridBuilder() override;

		uint16_t *GetCellSpotsList( unsigned gridCellNum, uint16_t *numCellSpots ) const override;

		void AddSpot( const vec3_t origin, uint16_t spotNum );
		void AddSpotToGridList( unsigned gridCellNum, uint16_t spotNum );
		void CopyTo( PrecomputedSpotsGrid *precomputedGrid );
	};

	PrecomputedSpotsGrid spotsGrid;

	static TacticalSpotsRegistry *instance;

	TacticalSpotsRegistry(): spotsGrid( this ) {}
public:

	~TacticalSpotsRegistry();

	bool Load( const char *mapname );

private:
	bool TryLoadPrecomputedData( const char *mapname );
	void SavePrecomputedData( const char *mapname );

	SpotsQueryVector &FindSpotsInRadius( const OriginParams &originParams, uint16_t *insideSpotNum ) const {
		return spotsGrid.FindSpotsInRadius( originParams, insideSpotNum );
	}
public:
	// TacticalSpotsRegistry should be init and shut down explicitly
	// (a game library is not unloaded when a map changes)
	static bool Init( const char *mapname );
	static void Shutdown();

	inline bool IsLoaded() const { return spots != nullptr && numSpots > 0; }

	static inline const TacticalSpotsRegistry *Instance() {
		return ( instance && instance->IsLoaded() ) ? instance : nullptr;
	}

	static inline void GetSpotsWalkabilityTraceBounds( vec3_t mins, vec3_t maxs ) {
		// This step size is rather huge but produces satisfiable results espectially on inclined surfaces
		VectorSet( mins, -2, -2, AI_STEPSIZE + 4 );
		VectorSet( maxs, +2, +2, +2 );
		VectorAdd( mins, playerbox_stand_mins, mins );
		VectorAdd( maxs, playerbox_stand_maxs, maxs );
	}

	int TravelTimeFromAreaToSpot( int areaNum, int spotNum ) const {
		assert( (unsigned)areaNum < (unsigned)AiAasWorld::Instance()->NumAreas() );
		assert( (unsigned)spotNum < (unsigned)numSpots );
		return spotsAndAreasTravelTimeTable[2 * ( areaNum * numSpots + spotNum ) + 1];
	}

	int TravelTimeFromSpotToArea( int spotNum, int areaNum ) const {
		assert( (unsigned)areaNum < (unsigned)AiAasWorld::Instance()->NumAreas() );
		assert( (unsigned)spotNum < (unsigned)numSpots );
		return spotsAndAreasTravelTimeTable[2 * ( areaNum * numSpots + spotNum ) + 0];
	}
};

#endif
