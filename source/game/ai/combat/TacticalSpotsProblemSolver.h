#ifndef QFUSION_TACTICALSPOTPROBLEMSOLVERS_H
#define QFUSION_TACTICALSPOTPROBLEMSOLVERS_H

#include "../bot.h"
#include "TacticalSpotsRegistry.h"

class TacticalSpotsProblemSolver {
public:
	typedef TacticalSpotsRegistry::TacticalSpot TacticalSpot;
	typedef TacticalSpotsRegistry::OriginParams OriginParams;
	typedef TacticalSpotsRegistry::SpotAndScore SpotAndScore;
	typedef TacticalSpotsRegistry::SpotsQueryVector SpotsQueryVector;
	typedef TacticalSpotsRegistry::SpotsAndScoreVector SpotsAndScoreVector;

	static constexpr auto MAX_SPOTS = TacticalSpotsRegistry::MAX_SPOTS;

	class BaseProblemParams {
		friend class TacticalSpotsProblemSolver;
		friend class AdvantageProblemSolver;
		friend class DodgeHazardProblemSolver;
	protected:
		const TrackedEnemy *enemiesListHead { nullptr };
		const TrackedEnemy *ignoredEnemy { nullptr };
		float enemiesInfluence { 0.75f };
		unsigned lastSeenEnemyMillisThreshold { 5000 };

		float minHeightAdvantageOverOrigin { 0.0f };
		float originWeightFalloffDistanceRatio { 0.0f };
		float originDistanceInfluence { 0.9f };
		float travelTimeInfluence { 0.9f };
		float heightOverOriginInfluence { 0.9f };
		int maxFeasibleTravelTimeMillis { 5000 };
		float spotProximityThreshold { 64.0f };
		bool checkToAndBackReach { false };

	public:
		void SetCheckToAndBackReach( bool checkToAndBack ) {
			this->checkToAndBackReach = checkToAndBack;
		}

		void SetOriginWeightFalloffDistanceRatio( float ratio ) {
			originWeightFalloffDistanceRatio = Clamp( ratio );
		}

		void SetMinHeightAdvantageOverOrigin( float minHeight ) {
			minHeightAdvantageOverOrigin = minHeight;
		}

		void SetMaxFeasibleTravelTimeMillis( int millis ) {
			maxFeasibleTravelTimeMillis = std::max( 1, millis );
		}

		void SetOriginDistanceInfluence( float influence ) { originDistanceInfluence = Clamp( influence ); }

		void SetTravelTimeInfluence( float influence ) { travelTimeInfluence = Clamp( influence ); }

		void SetHeightOverOriginInfluence( float influence ) { heightOverOriginInfluence = Clamp( influence ); }

		void SetSpotProximityThreshold( float radius ) { spotProximityThreshold = std::max( 0.0f, radius ); }

		/**
		 * While blocking of positions by enemies to some degree is handled
		 * implicitly by the router we need more reasoning about a "good" position.
		 * Tactical spots that are less visible for enemies are preferred
		 * so a bot is less likely to be shot in its back.
		 * @param listHead_ a list of all tracked enemies of the bot.
		 * @param ignoredEnemy_ an enemy that should be excluded from obstruction tests (usually a primary enemy)
		 * @param influence_ an influence of the obstruction/visibility factor on a spot score.
		 * @param lastSeenMillisThreshold_ enemies last seen earlier are not taken into account.
		 */
		void TakeEnemiesIntoAccount( const TrackedEnemy *listHead_,
									 const TrackedEnemy *ignoredEnemy_,
									 float influence_ = 0.5f,
									 unsigned lastSeenMillisThreshold_ = 3000u ) {
			this->enemiesListHead = listHead_;
			this->ignoredEnemy = ignoredEnemy_;
			this->enemiesInfluence = Clamp( influence_ );
			this->lastSeenEnemyMillisThreshold = lastSeenMillisThreshold_;
		}
	};

protected:
	const OriginParams &originParams;
	TacticalSpotsRegistry *const tacticalSpotsRegistry;

	virtual SpotsAndScoreVector &SelectCandidateSpots( const SpotsQueryVector &spotsFromQuery );

	virtual SpotsAndScoreVector &CheckSpotsReachFromOrigin( SpotsAndScoreVector &candidateSpots,
															uint16_t insideSpotNum );

	virtual SpotsAndScoreVector &CheckSpotsReachFromOriginAndBack( SpotsAndScoreVector &candidateSpots,
																   uint16_t insideSpotNum );

	SpotsAndScoreVector &CheckSpotsReach( SpotsAndScoreVector &candidateSpots, uint16_t insideSpotNum ) {
		if( problemParams.checkToAndBackReach ) {
			return CheckSpotsReachFromOriginAndBack( candidateSpots, insideSpotNum );
		}
		return CheckSpotsReachFromOrigin( candidateSpots, insideSpotNum );
	}

	virtual SpotsAndScoreVector &CheckEnemiesInfluence( SpotsAndScoreVector &candidateSpots );

	int CleanupAndCopyResults( SpotsAndScoreVector &spots, vec3_t *spotOrigins, int maxSpots ) {
		return CleanupAndCopyResults( ArrayRange<SpotAndScore>( spots.begin(), spots.end()), spotOrigins, maxSpots );
	}

	virtual int CleanupAndCopyResults( const ArrayRange<SpotAndScore> &spotsRange, vec3_t *spotOrigins, int maxSpots );
private:
	const BaseProblemParams &problemParams;
public:
	TacticalSpotsProblemSolver( const OriginParams &originParams_, const BaseProblemParams &problemParams_ )
		: originParams( originParams_ )
		, tacticalSpotsRegistry( TacticalSpotsRegistry::instance )
		, problemParams( problemParams_ ) {}

	virtual ~TacticalSpotsProblemSolver() = default;

	virtual bool FindSingle( vec3_t spot ) {
		// Assume an address of array of spot origins is the address of the first component of the single vec3_t param
		return FindMany( (vec3_t *)&spot[0], 1 ) == 1;
	}

	virtual int FindMany( vec3_t *spots, int maxSpots ) = 0;
};





#endif
