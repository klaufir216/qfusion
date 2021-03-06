#include "TacticalSpotsProblemSolver.h"
#include "SpotsProblemSolversLocal.h"
#include "../navigation/AasElementsMask.h"
#include "../ai_ground_trace_cache.h"

SpotsAndScoreVector &TacticalSpotsProblemSolver::SelectCandidateSpots( const SpotsQueryVector &spotsFromQuery ) {
	const float minHeightAdvantageOverOrigin = problemParams.minHeightAdvantageOverOrigin;
	const float heightOverOriginInfluence = problemParams.heightOverOriginInfluence;
	const float searchRadius = originParams.searchRadius;
	const float originZ = originParams.origin[2];
	const auto *spots = tacticalSpotsRegistry->spots;
	// Copy to stack for faster access
	Vec3 origin( originParams.origin );

	SpotsAndScoreVector &result = tacticalSpotsRegistry->temporariesAllocator.GetNextCleanSpotsAndScoreVector();

	for( auto spotNum: spotsFromQuery ) {
		const TacticalSpot &spot = spots[spotNum];

		float heightOverOrigin = spot.absMins[2] - originZ;
		if( heightOverOrigin < minHeightAdvantageOverOrigin ) {
			continue;
		}

		float squareDistanceToOrigin = DistanceSquared( origin.Data(), spot.origin );
		if( squareDistanceToOrigin > searchRadius * searchRadius ) {
			continue;
		}

		float score = 1.0f;
		float factor = BoundedFraction( heightOverOrigin - minHeightAdvantageOverOrigin, searchRadius );
		score = ApplyFactor( score, factor, heightOverOriginInfluence );

		result.push_back( SpotAndScore( spotNum, score ) );
	}

	// Sort result so best score areas are first
	std::sort( result.begin(), result.end() );
	return result;
}

SpotsAndScoreVector &TacticalSpotsProblemSolver::CheckSpotsReachFromOrigin( SpotsAndScoreVector &candidateSpots ) {
	const auto *const routeCache = originParams.routeCache;
	const float *const origin = originParams.origin;
	const auto *const spots = tacticalSpotsRegistry->spots;

	const int originAreaNum = originParams.originAreaNum;
	const float searchRadius = originParams.searchRadius;
	// AAS uses travel time in centiseconds
	const int maxFeasibleTravelTimeCentis = problemParams.maxFeasibleTravelTimeMillis / 10;
	const float weightFalloffDistanceRatio = problemParams.originWeightFalloffDistanceRatio;
	const float distanceInfluence = problemParams.originDistanceInfluence;
	const float travelTimeInfluence = problemParams.travelTimeInfluence;
	const auto travelFlags = Bot::ALLOWED_TRAVEL_FLAGS;

	SpotsAndScoreVector &result = tacticalSpotsRegistry->temporariesAllocator.GetNextCleanSpotsAndScoreVector();

	constexpr const char *tag = "TacticalSpotsProblemSolver::CheckSpotsReachFromOrigin()";
#ifndef PUBLIC_BUILD
	static constexpr bool checkTravelTimes = true;
#else
	static constexpr bool checkTravelTimes = false;
#endif

	// The outer index of the table corresponds to an area to aid cache-friendly iteration in these checks
	for( const SpotAndScore &spotAndScore: candidateSpots ) {
		const TacticalSpot &spot = spots[spotAndScore.spotNum];
		// If zero the spot should be considered a-priori non-reachable from the origin area.
		// The same applies to the upper travel time bounds

		// Area-to-spot time is the first element in the pair
		const int tableTravelTime = tacticalSpotsRegistry->TravelTimeFromAreaToSpot( originAreaNum, spotAndScore.spotNum );
		if( !tableTravelTime || tableTravelTime > maxFeasibleTravelTimeCentis ) {
			// TODO: Being strict with checks we should test whether an actual travel time is defined
			continue;
		}

		// Get an actual travel time using the route cache that is very likely has additional restrictions
		const int travelTime = routeCache->TravelTimeToGoalArea( originAreaNum, spot.aasAreaNum, travelFlags );
		// Check travel times if we are using the same travel flags the table is compiled for
		if( checkTravelTimes && ( travelFlags == Bot::ALLOWED_TRAVEL_FLAGS ) ) {
			// The actual travel time may be undefined or greater than the table one
			// due to excluding areas from routing but the table time must not be greater
			if( travelTime && travelTime < tableTravelTime ) {
				const char *format = "The table travel time %d > actual one %d for traveling from area %d to spot %d\n";
				AI_FailWith( tag, format, tableTravelTime, travelTime, originAreaNum, spotAndScore.spotNum );
			}
		}

		if( !travelTime || travelTime > maxFeasibleTravelTimeCentis ) {
			continue;
		}

		const float travelTimeFactor = 1.0f - ComputeTravelTimeFactor( travelTime, maxFeasibleTravelTimeCentis );
		const float distanceFactor = ComputeDistanceFactor( spot.origin, origin, weightFalloffDistanceRatio, searchRadius );
		float newScore = spotAndScore.score;
		newScore = ApplyFactor( newScore, distanceFactor, distanceInfluence );
		newScore = ApplyFactor( newScore, travelTimeFactor, travelTimeInfluence );
		result.push_back( SpotAndScore( spotAndScore.spotNum, newScore ) );
	}

	// Sort result so best score areas are first
	std::sort( result.begin(), result.end() );
	return result;
}

SpotsAndScoreVector &TacticalSpotsProblemSolver::CheckSpotsReachFromOriginAndBack( SpotsAndScoreVector &candidateSpots ) {
	const auto *const routeCache = originParams.routeCache;
	const float *const origin = originParams.origin;
	const auto *const spots = tacticalSpotsRegistry->spots;

	const int originAreaNum = originParams.originAreaNum;
	const float searchRadius = originParams.searchRadius;
	// AAS uses time in centiseconds
	const int maxFeasibleTravelTimeCentis = problemParams.maxFeasibleTravelTimeMillis / 10;
	const float weightFalloffDistanceRatio = problemParams.originWeightFalloffDistanceRatio;
	const float distanceInfluence = problemParams.originDistanceInfluence;
	const float travelTimeInfluence = problemParams.travelTimeInfluence;
	const auto travelFlags = Bot::ALLOWED_TRAVEL_FLAGS;

	SpotsAndScoreVector &result = tacticalSpotsRegistry->temporariesAllocator.GetNextCleanSpotsAndScoreVector();
	constexpr const char *tag = "TacticalSpotsProblemSolver::CheckSpotsReachFromOriginAndBack()";
#ifndef PUBLIC_BUILD
	static constexpr bool checkTravelTimes = true;
#else
	static constexpr bool checkTravelTimes = false;
#endif

	// The outer index of the table corresponds to an area to aid cache-friendly iteration in these checks
	for( const SpotAndScore &spotAndScore : candidateSpots ) {
		const auto spotNum = spotAndScore.spotNum;
		const TacticalSpot &spot = spots[spotNum];
		const int tableToTravelTime = tacticalSpotsRegistry->TravelTimeFromAreaToSpot( originAreaNum, spotNum );
		if( !tableToTravelTime ) {
			// TODO: Being strict with checks we should test whether an actual travel time is defined
			continue;
		}
		const int tableBackTravelTime = tacticalSpotsRegistry->TravelTimeFromSpotToArea( spotNum, originAreaNum );
		if( !tableBackTravelTime ) {
			// TODO: Being strict with checks we should test whether an actual travel time is defined
			continue;
		}
		// A round trip time can't be 2x larger
		if( tableToTravelTime + tableBackTravelTime > 2 * maxFeasibleTravelTimeCentis ) {
			// TODO: Being strict with checks we should test whether actual travel times are not less
			continue;
		}

		// Get an actual travel time (non-zero table values do not guarantee reachability)
		const int toTravelTime = routeCache->TravelTimeToGoalArea( originAreaNum, spot.aasAreaNum, travelFlags );
		// Check travel times if we are using the same travel flags the table is compiled for
		if( checkTravelTimes && ( travelFlags == Bot::ALLOWED_TRAVEL_FLAGS ) ) {
			if( toTravelTime && toTravelTime < tableToTravelTime ) {
				const char *format = "The table travel time %d > actual one %d for traveling from area %d to spot %d\n";
				AI_FailWith( tag, format, tableToTravelTime, toTravelTime, originAreaNum, spotNum );
			}
		}

		// If `to` travel time is apriori greater than maximum allowed one (and thus the sum would be), reject early.
		if( !toTravelTime || toTravelTime > maxFeasibleTravelTimeCentis ) {
			continue;
		}

		const int backTravelTime = routeCache->TravelTimeToGoalArea( spot.aasAreaNum, originAreaNum, travelFlags );
		if( checkTravelTimes && ( travelFlags == Bot::ALLOWED_TRAVEL_FLAGS ) ) {
			if( backTravelTime && backTravelTime < tableBackTravelTime ) {
				const char *format = "The table travel time %d > actual %d one for traveling from spot %d to area %d\n";
				AI_FailWith( tag, format, tableBackTravelTime, backTravelTime, spotNum, originAreaNum );
			}
		}

		if( !backTravelTime || toTravelTime + backTravelTime > 2 * maxFeasibleTravelTimeCentis ) {
			continue;
		}

		const int totalTravelTimeCentis = toTravelTime + backTravelTime;
		const float travelTimeFactor = ComputeTravelTimeFactor( totalTravelTimeCentis, maxFeasibleTravelTimeCentis );
		const float distanceFactor = ComputeDistanceFactor( spot.origin, origin, weightFalloffDistanceRatio, searchRadius );
		float newScore = spotAndScore.score;
		newScore = ApplyFactor( newScore, distanceFactor, distanceInfluence );
		newScore = ApplyFactor( newScore, travelTimeFactor, travelTimeInfluence );
		result.push_back( SpotAndScore( spotAndScore.spotNum, newScore ) );
	}

	// Sort result so best score areas are first
	std::sort( result.begin(), result.end() );
	return result;
}

SpotsAndScoreVector &TacticalSpotsProblemSolver::CheckEnemiesInfluence( SpotsAndScoreVector &candidateSpots ) {
	if( candidateSpots.empty() ) {
		return candidateSpots;
	}
	if( !problemParams.enemiesListHead ) {
		return candidateSpots;
	}
	if( problemParams.enemiesInfluence <= 0.0f ) {
		return candidateSpots;
	}

	// Precompute some enemy parameters that are going to be used in an inner loop.

	struct CachedEnemyData {
		const bool *areaVisRow;
		vec3_t viewOrigin;
		vec3_t lookDir;
		vec3_t velocityDir2D;
		float speed2D;
		int groundedAreaNum;
	};

	// Pick at most as many enemies as the number of AAS tmp rows we can allocate
	StaticVector<CachedEnemyData, AasElementsMask::TMP_ROW_REDUNDANCY_SCALE> cachedEnemyData;

	const auto *aasWorld = AiAasWorld::Instance();
	const int64_t levelTime = level.time;

	for( const TrackedEnemy *enemy = problemParams.enemiesListHead; enemy; enemy = enemy->NextInTrackedList() ) {
		if( levelTime - enemy->LastSeenAt() > problemParams.lastSeenEnemyMillisThreshold ) {
			continue;
		}
		// If the enemy has been invalidated but not unlinked yet (todo: is it reachable?)
		if( !enemy->IsValid() ) {
			continue;
		}
		// If it seems to be a primary enemy
		if( enemy == problemParams.ignoredEnemy ) {
			continue;
		}

		bool *const areaVisRow = AasElementsMask::TmpAreasVisRow( (int)cachedEnemyData.size() );
		CachedEnemyData *const enemyData = cachedEnemyData.unsafe_grow_back();

		Vec3 enemyOrigin( enemy->LastSeenOrigin() );
		enemyOrigin.CopyTo( enemyData->viewOrigin );
		enemyData->viewOrigin[2] += playerbox_stand_viewheight;
		enemy->LookDir().CopyTo( enemyData->lookDir );
		enemy->LastSeenVelocity().CopyTo( enemyData->velocityDir2D );
		enemyData->velocityDir2D[2] = 0;
		enemyData->speed2D = VectorLengthSquared( enemyData->velocityDir2D );
		if( enemyData->speed2D > 0.001f ) {
			enemyData->speed2D = std::sqrt( enemyData->speed2D );
			float scale = 1.0f / enemyData->speed2D;
			VectorScale( enemyData->velocityDir2D, scale, enemyData->velocityDir2D );
		}

		if( enemy->ent->ai && enemy->ent->ai->botRef ) {
			int areaNums[2] = { 0, 0 };
			enemy->ent->ai->botRef->EntityPhysicsState()->PrepareRoutingStartAreas( areaNums );
			// TODO: PrepareRoutingStartAreas() should always put grounded area first.
			// The currently saved data is a valid input for further tests but could lead to false negatives.
			enemyData->groundedAreaNum = areaNums[0];
		} else {
			vec3_t tmpOrigin;
			const float *testedOrigin = enemyOrigin.Data();
			if( AiGroundTraceCache::Instance()->TryDropToFloor( enemy->ent, 64.0f, tmpOrigin ) ) {
				testedOrigin = tmpOrigin;
			}
			enemyData->groundedAreaNum = aasWorld->FindAreaNum( testedOrigin );
		}

		enemyData->areaVisRow = aasWorld->DecompressAreaVis( enemyData->groundedAreaNum, areaVisRow );

		// Interrupt if the capacity is exceeded. This is not really correct
		// since the enemies are not sorted starting from the most dangerous one
		// but fits realistic situations well. The gameplay is a mess otherwise anyway.
		if( cachedEnemyData.size() == cachedEnemyData.capacity() ) {
			break;
		}
	}

	if( cachedEnemyData.empty() ) {
		return candidateSpots;
	}

	const auto *const spots = tacticalSpotsRegistry->spots;
	for( auto &spotAndScore: candidateSpots ) {
		const auto &__restrict spot = spots[spotAndScore.spotNum];
		const int spotFloorClusterNum = aasWorld->AreaFloorClusterNums()[spot.aasAreaNum];
		float spotVisScore = 0.0f;
		for( const CachedEnemyData &enemyData: cachedEnemyData ) {
			Vec3 toSpotDir( spot.origin );
			toSpotDir -= enemyData.viewOrigin;
			float squareDistanceToSpot = toSpotDir.SquaredLength();
			// Skip far enemies
			if( squareDistanceToSpot > 1000 * 1000 ) {
				continue;
			}
			// Skip not very close enemies that are seemingly running away from spot
			if( squareDistanceToSpot > 384 * 384 ) {
				toSpotDir *= Q_RSqrt( squareDistanceToSpot );
				if( toSpotDir.Dot( enemyData.lookDir ) < 0 ) {
					if( enemyData.speed2D >= DEFAULT_PLAYERSPEED ) {
						if( toSpotDir.Dot( enemyData.velocityDir2D ) < 0 ) {
							continue;
						}
					}
				}
			}

			// If the spot is very unlikely to be in enemy view
			// Reasons why it still may be (but we don't care):
			// 1) A spot can really occupy > 1 area
			// 2) An enemy can really occupy > 1 area
			// 3) AAS vis table is built using fairly coarse tests.
			if( !enemyData.areaVisRow[spot.aasAreaNum] ) {
				continue;
			}

			// If the spot and the enemy are in the same floor cluster
			if( spotFloorClusterNum && spotFloorClusterNum == enemyData.groundedAreaNum ) {
				if( !aasWorld->IsAreaWalkableInFloorCluster( enemyData.groundedAreaNum, spotFloorClusterNum ) ) {
					continue;
				}
			} else {
				trace_t trace;
				SolidWorldTrace( &trace, enemyData.viewOrigin, spot.origin );
				if( trace.fraction != 1.0f ) {
					continue;
				}
			}

			// Just add a unit on influence for every enemy.
			// We can't fully predict enemy future state
			// (e.g. whether it can become very dangerous by picking something).
			// Even a weak enemy can do a substantial damage since unless we take it into account.
			// We just select spots that are less visible for other enemies for proper positioning.
			spotVisScore += 1.0f;
		}

		// We have computed a vis score testing every enemy.
		// Now modify the original score
		const float enemyInfluenceFactor = Q_Rcp( 1.0f + spotVisScore );
		spotAndScore.score = ApplyFactor( spotAndScore.score, enemyInfluenceFactor, problemParams.enemiesInfluence );
	}

	std::sort( candidateSpots.begin(), candidateSpots.end() );

	return candidateSpots;
}

int TacticalSpotsProblemSolver::CleanupAndCopyResults( const ArrayRange<SpotAndScore> &spotsRange,
													   vec3_t *spotOrigins, int maxSpots ) {
	const auto resultsSize = (unsigned)spotsRange.size();
	if( maxSpots == 0 || resultsSize == 0 ) {
		tacticalSpotsRegistry->temporariesAllocator.Release();
		return 0;
	}

	const auto *const spots = tacticalSpotsRegistry->spots;
	const auto *const spotsAndScores = spotsRange.begin();

	// Its a common case so give it an optimized branch
	if( maxSpots == 1 ) {
		VectorCopy( spots[spotsAndScores[0].spotNum].origin, spotOrigins[0] );
		tacticalSpotsRegistry->temporariesAllocator.Release();
		return 1;
	}

	const float squareProximityThreshold = problemParams.spotProximityThreshold * problemParams.spotProximityThreshold;
	bool *const isSpotExcluded = tacticalSpotsRegistry->temporariesAllocator.GetCleanExcludedSpotsMask();

	int numSpots_ = 0;
	unsigned keptSpotIndex = 0;
	for(;; ) {
		if( keptSpotIndex >= resultsSize ) {
			tacticalSpotsRegistry->temporariesAllocator.Release();
			return numSpots_;
		}
		if( numSpots_ >= maxSpots ) {
			tacticalSpotsRegistry->temporariesAllocator.Release();
			return numSpots_;
		}

		// Spots are sorted by score.
		// So first spot not marked as excluded yet has higher priority and should be kept.
		// The condition that terminates the outer loop ensures we have a valid kept spot.
		const TacticalSpot &keptSpot = spots[spotsAndScores[keptSpotIndex].spotNum];
		VectorCopy( keptSpot.origin, spotOrigins[numSpots_] );
		++numSpots_;

		// Start from the next spot of the kept one
		unsigned testedSpotIndex = keptSpotIndex + 1;
		// Reset kept spot index so the loop is going to terminate next step by default
		keptSpotIndex = std::numeric_limits<unsigned>::max();
		// For every remaining spot in results left
		for(; testedSpotIndex < resultsSize; testedSpotIndex++ ) {
			// Skip already excluded spots
			if( isSpotExcluded[testedSpotIndex] ) {
				continue;
			}

			const TacticalSpot &testedSpot = spots[spotsAndScores[testedSpotIndex].spotNum];
			if( DistanceSquared( keptSpot.origin, testedSpot.origin ) < squareProximityThreshold ) {
				isSpotExcluded[testedSpotIndex] = true;
			} else if( keptSpotIndex > testedSpotIndex ) {
				// Mark the first non-excluded next spot for the outer loop
				keptSpotIndex = testedSpotIndex;
			}
		}
	}
}