#ifndef QFUSION_SAMEFLOORCLUSTERAREASCACHE_H
#define QFUSION_SAMEFLOORCLUSTERAREASCACHE_H

#include "../ai_local.h"
#include "../static_vector.h"

class Bot;
struct Hazard;
class MovementPredictionContext;

class FloorClusterAreasCache {
protected:
	static constexpr unsigned HEAP_SIZE = 20;

	using CandidateAreasHeap = StaticVector<AreaAndScore, HEAP_SIZE>;

	/**
	 * If a bot remains in the same area candidates computation might be skipped
	 * and only straight-line walkability tests are to be performed.
	 */
	mutable CandidateAreasHeap oldCandidatesHeap;

	Bot *const bot;
	const AiAasWorld *aasWorld;

	const float areaSelectionNearThreshold;
	const float areaSelectionFarThreshold;

	mutable int64_t computedAt { 0 };
	mutable Vec3 computedTargetAreaPoint { 0, 0, 0 };
	mutable int computedForAreaNum { 0 };
	mutable int computedTargetAreaNum { 0 };
	mutable int computedTravelTime { 0 };

	virtual ~FloorClusterAreasCache() = default;

	FloorClusterAreasCache( Bot *bot_, float areaSelectionNearThreshold_, float areaSelectionFarThreshold_ )
		: bot( bot_ )
		, aasWorld( AiAasWorld::Instance() )
		, areaSelectionNearThreshold( areaSelectionNearThreshold_ )
		, areaSelectionFarThreshold( areaSelectionFarThreshold_ ) {
		assert( areaSelectionNearThreshold_ > 0.0f );
		assert( areaSelectionFarThreshold_ > areaSelectionNearThreshold_ );
	}

	/**
	 * Given a list of areas in cluster, builds a heap of areas
	 * that closest to target areas get evicted first.
	 * @param context a movement prediction context
	 * @param maxTravelTimeThreshold skip areas if the travel time from area to target is not better than this
	 * @param clusterAreaNums an array of area numbers of the floor cluster
	 * @param numClusterAreas a number of areas in the floor cluster
	 * @param result the heap being built
	 */
	void BuildCandidateAreasHeap( MovementPredictionContext *context,
								  int maxTravelTimeThreshold,
								  const uint16_t *clusterAreaNums,
								  int numClusterAreas,
								  CandidateAreasHeap &result ) const;

	/**
	 * Selects candidate areas without actually making a heap.
	 * The number of areas in the cluster must not be greater than the heap capacity.
	 * @param context a movement prediction context
	 * @param hazardToEvade a hazard that may have impact on areas, nullable
	 * @param maxTravelTimeThreshold skip areas if the travel time from area to target is not better than this
	 * @param clusterAreaNums an array of area numbers of the floor cluster
	 * @param numClusterAreas a number of areas in the floor cluster
	 * @param result selected candidate areas
	 */
	void PrepareAreasForSmallCluster( MovementPredictionContext *__restrict context,
									  const Hazard *__restrict hazardToEvade,
									  int maxTravelTimeThreshold,
								      const uint16_t *__restrict clusterAreaNums,
								      int numClusterAreas,
								      CandidateAreasHeap &__restrict result ) const;

	/**
	 * Selects candidate areas without actually making a heap.
	 * The number of areas in the cluster is assumed to be greater than the heap capacity.
	 * @param context a movement prediction context
	 * @param hazardToEvade a hazard that may have impact on areas, nullable
	 * @param maxTravelTimeThreshold skip areas if the travel time from area to target is not better than this
	 * @param clusterAreaNums an array of area numbers of the floor cluster
	 * @param numClusterAreas a number of areas in the floor cluster
	 * @param result selected candidate areas
	 */
	void PrepareAreasForLargeCluster( MovementPredictionContext *__restrict context,
		                              const Hazard *__restrict hazardToEvade,
		                              int maxTravelTimeThreshold,
		                              const uint16_t *__restrict clusterAreaNums,
		                              int numClusterAreas,
		                              CandidateAreasHeap &__restrict result ) const;

	virtual bool NeedsToBeComputed( MovementPredictionContext *context ) const = 0;

	/**
	 * Actually tries to find a closest to target point.
	 * Must be overridden in descendants.
	 * @param context a movement prediction context
	 * @param areaNum the best area num
	 * @return an AAS travel time from the best area to target, zero on failure.
	 */
	virtual int FindClosestToTargetPoint( MovementPredictionContext *context, int *areaNum ) const = 0;

	/**
	 * Tries to populate an algorithm scratchpad heap
	 * by values of the cached cluster areas heap.
	 * @param context a movement prediction context
	 * @param currGroundedAreaNum a current grounded area number at the current prediction frame
	 * @param expectedClusterNum an actual cluster we are interested in
	 * @param scratchpadHeap an algorithm scratchpad heap that is going to be used for retrieval of best next candidate
	 */
	void TryReusingOldHeap( MovementPredictionContext *context,
							int currGroundedAreaNum,
							int expectedClusterNum,
							CandidateAreasHeap &scratchpadHeap ) const;

	inline bool AreaPassesCollisionTest( MovementPredictionContext *context, int areaNum ) const;

	virtual bool AreaPassesCollisionTest( const Vec3 &start, int areaNum, const vec3_t mins, const vec3_t maxs ) const = 0;
public:
	/**
	 * Gets a cached closest to target point (if any).
	 * @param context a movement prediction context
	 * @param resultPoint the origin of the best point
	 * @param areaNum an address to write the number of the best area if specified
	 * @return an AAS travel time from the best area to target, zero on failure
	 */
	int GetClosestToTargetPoint( MovementPredictionContext *context, float *resultPoint, int *areaNum = nullptr ) const;
};

class SameFloorClusterAreasCache final : public FloorClusterAreasCache {
	// If an area is closer to a bot, it should be considered reached
	static constexpr float REACHABILITY_RADIUS = 128.0f;

	int FindClosestToTargetPoint( MovementPredictionContext *context, int *areaNum ) const override;

	bool NeedsToBeComputed( MovementPredictionContext *context ) const override;

	bool AreaPassesCollisionTest( const Vec3 &start, int areaNum, const vec3_t mins, const vec3_t maxs ) const override;
public:
	explicit SameFloorClusterAreasCache( Bot *bot_ )
		: FloorClusterAreasCache( bot_, 192.0f, 768.0f ) {}
};

class NextFloorClusterAreasCache final : public FloorClusterAreasCache {
	int FindClosestToTargetPoint( MovementPredictionContext *context, int *areaNum ) const override;

	bool NeedsToBeComputed( MovementPredictionContext *context ) const override;

	bool AreaPassesCollisionTest( const Vec3 &start, int areaNum, const vec3_t mins, const vec3_t maxs ) const override;
public:
	explicit NextFloorClusterAreasCache( Bot *bot_ )
		: FloorClusterAreasCache( bot_, 64.0f, 1024.0f ) {}
};

#endif
