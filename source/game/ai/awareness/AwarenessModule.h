#ifndef QFUSION_BOT_THREAT_TRACKER_H
#define QFUSION_BOT_THREAT_TRACKER_H

#include "AlertTracker.h"
#include "EnemiesTracker.h"
#include "SelectedEnemies.h"
#include "HazardsSelector.h"
#include "EventsTracker.h"
#include "KeptInFovPointTracker.h"
#include "PathBlockingTracker.h"

class AiSquad;
class Bot;

class BotAwarenessModule: public AiFrameAwareUpdatable {
	friend class Bot;

	AiEnemiesTracker *activeEnemiesTracker { &ownEnemiesTracker };
	AiSquad *squad { nullptr };
	Bot *const bot;

	SelectedEnemies &selectedEnemies;
	SelectedEnemies &lostEnemies;

	const unsigned targetChoicePeriod;
	const unsigned reactionTime;

	bool shouldUpdateBlockedAreasStatus { false };

	AlertTracker alertTracker;
	HazardsDetector hazardsDetector;
	HazardsSelector hazardsSelector;
	EventsTracker eventsTracker;
	KeptInFovPointTracker keptInFovPointTracker;
	PathBlockingTracker pathBlockingTracker;
public:
	struct HurtEvent: public Selection {
		// Initialize the inflictor by the world entity (it is never valid as one).
		// This helps to avoid extra branching from testing for nullity.
		const edict_t *inflictor { world };
		int64_t lastHitTimestamp { 0 };
		Vec3 possibleOrigin { 0, 0, 0 };
		float totalDamage { 0.0f };

		bool IsValidFor( const Bot *bot ) const;

		void Invalidate() {
			// We used to set zero timestamp but the timestamp acts as an instance id now
			// and we must comply to the instance id contract
			// (it remains the same for an invalidated Selection).
			inflictor = world;
		}

		unsigned InstanceId() const override {
			// This code is aware of unsigned range overflow... even if it should not realistically happen.
			return (unsigned)( lastHitTimestamp % std::numeric_limits<unsigned>::max() );
		}

		bool ValidAsSelection() const override {
			// This is just to comply to the Selection interface.
			// Return true if the hurt event has not been invalidated.
			return inflictor != world;
		}
	};

private:
	mutable HurtEvent hurtEvent;

	class EnemiesTracker : public AiEnemiesTracker {
		Bot *const bot;
		BotAwarenessModule *const module;
	protected:
		void OnHurtByNewThreat( const edict_t *newThreat ) override;
		bool CheckHasQuad() const override;
		bool CheckHasShell() const override;
		float ComputeDamageToBeKilled() const override;
		void OnEnemyRemoved( const TrackedEnemy *enemy ) override;
		void SetBotRoleWeight( const edict_t *bot_, float weight ) override {}
		float GetAdditionalEnemyWeight( const edict_t *bot_, const edict_t *enemy ) const override { return 0; }
		void OnBotEnemyAssigned( const edict_t *bot_, const TrackedEnemy *enemy ) override {}
	public:
		EnemiesTracker( Bot *bot_, BotAwarenessModule *module_ );
	};

	EnemiesTracker ownEnemiesTracker;

	Hazard triggeredPlanningHazard { nullptr };

	void Frame() override;
	void Think() override;

	void SetFrameAffinity( unsigned modulo, unsigned offset ) override {
		AiFrameAwareUpdatable::SetFrameAffinity( modulo, offset );
		eventsTracker.SetFrameAffinity( modulo, offset );
		ownEnemiesTracker.SetFrameAffinity( modulo, offset );
	}

	void UpdateSelectedEnemies();
	void UpdateBlockedAreasStatus();
	void TryTriggerPlanningForNewHazard();

	void RegisterVisibleEnemies();

	void CheckForNewHazards();
public:
	BotAwarenessModule( Bot *bot_ );

	void OnAttachedToSquad( AiSquad *squad_ );
	void OnDetachedFromSquad( AiSquad *squad_ );

	void OnHurtByNewThreat( const edict_t *newThreat, const AiFrameAwareUpdatable *threatDetector );
	void OnEnemyRemoved( const TrackedEnemy *enemy );

	void OnEnemyViewed( const edict_t *enemy );
	void OnEnemyOriginGuessed( const edict_t *enemy, unsigned minMillisSinceLastSeen, const float *guessedOrigin = nullptr );

	void RegisterEvent( const edict_t *ent, int event, int parm ) {
		eventsTracker.RegisterEvent( ent, event, parm );
	}

	void OnPain( const edict_t *enemy, float kick, int damage );
	void OnEnemyDamaged( const edict_t *target, int damage );

	const TrackedEnemy *ChooseLostOrHiddenEnemy( unsigned timeout = ~0u );

	const TrackedEnemy *TrackedEnemiesHead() const {
		return ( (const AiEnemiesTracker *)activeEnemiesTracker )->TrackedEnemiesHead();
	}

	const Hazard *PrimaryHazard() const {
		if( const auto *hazard = hazardsSelector.PrimaryHazard() ) {
			// The return value must always be valid if present.
			// Check whether if has not been invalidated since last selection.
			if( hazard->IsValid() ) {
				return hazard;
			}
		}
		return nullptr;
	}

	const HurtEvent *GetValidHurtEvent() const {
		if( !hurtEvent.IsValidFor( bot ) ) {
			hurtEvent.Invalidate();
			return nullptr;
		}

		return &hurtEvent;
	}

	const float *GetKeptInFovPoint() const {
		return keptInFovPointTracker.GetActivePoint();
	}

	// In these calls use not active but bot's own enemy pool
	// (this behaviour is expected by callers, otherwise referring to a squad enemy pool is enough)
	inline int64_t LastAttackedByTime( const edict_t *attacker ) const {
		return ownEnemiesTracker.LastAttackedByTime( attacker );
	}

	inline int64_t LastTargetTime( const edict_t *target ) const {
		return ownEnemiesTracker.LastTargetTime( target );
	}

	void EnableAutoAlert( const AiAlertSpot &alertSpot,
						  AlertTracker::AlertCallback callback,
						  AiFrameAwareUpdatable *receiver ) {
		alertTracker.EnableAutoAlert( alertSpot, callback, receiver );
	}

	void DisableAutoAlert( int id ) {
		alertTracker.DisableAutoAlert( id );
	}
};

#endif
