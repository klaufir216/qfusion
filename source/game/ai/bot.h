#ifndef AI_BOT_H
#define AI_BOT_H

#include "static_vector.h"
#include "awareness/AwarenessModule.h"
#include "planning/PlanningModule.h"
#include "ai_base_ai.h"
#include "vec3.h"

#include "movement/MovementModule.h"
#include "combat/WeaponsUsageModule.h"
#include "planning/TacticalSpotsCache.h"
#include "awareness/AwarenessModule.h"
#include "planning/RoamingManager.h"
#include "bot_weight_config.h"

#include "planning/Goals.h"
#include "planning/Actions.h"

#include <functional>

class AiSquad;
class AiEnemiesTracker;

/**
 * This can be represented as an enum but feels better in the following form.
 * Many values that affect bot behaviour already are not boolean
 * (such as nav targets and special movement states like camping spots),
 * and thus controlling a bot by a single flags field already is not possible.
 * This struct is likely to be extended by non-boolean values later.
 */
struct SelectedMiscTactics {
	bool willAdvance;
	bool willRetreat;

	bool shouldBeSilent;
	bool shouldMoveCarefully;

	bool shouldAttack;
	bool shouldKeepXhairOnEnemy;

	bool willAttackMelee;
	bool shouldRushHeadless;

	SelectedMiscTactics() { Clear(); };

	void Clear() {
		willAdvance = false;
		willRetreat = false;

		shouldBeSilent = false;
		shouldMoveCarefully = false;

		shouldAttack = false;
		shouldKeepXhairOnEnemy = false;

		willAttackMelee = false;
		shouldRushHeadless = false;
	}

	void PreferAttackRatherThanRun() {
		shouldAttack = true;
		shouldKeepXhairOnEnemy = true;
	}

	void PreferRunRatherThanAttack() {
		shouldAttack = true;
		shouldKeepXhairOnEnemy = false;
	}
};

struct AiObjectiveSpot;
struct AiDefenceSpot;
struct AiOffenseSpot;

class Bot: public Ai {
	friend class AiManager;
	friend class BotEvolutionManager;
	friend class AiBaseTeam;
	friend class AiSquadBasedTeam;
	friend class AiObjectiveBasedTeam;
	friend class BotPlanner;
	friend class AiSquad;
	friend class SquadsBuilder;
	friend class AiEnemiesTracker;
	friend class PathBlockingTracker;
	friend class BotAwarenessModule;
	friend class BotFireTargetCache;
	friend class BotItemsSelector;
	friend class BotWeaponSelector;
	friend class BotWeaponsUsageModule;
	friend class BotRoamingManager;
	friend class TacticalSpotsRegistry;
	friend class BotNavMeshQueryCache;
	friend class BotTacticalSpotsCache;
	friend class BotRoamGoal;
	friend class WorldState;

	friend class BotMovementModule;
	friend class MovementPredictionContext;
	// TODO: Remove this and refactor "kept in fov point" handling
	friend class FallbackMovementAction;
	friend class CorrectWeaponJumpAction;

	friend class CachedTravelTimesMatrix;

	template <typename T> friend T *Link( T *, T **, int );
	template <typename T> friend T *Unlink( T *, T **, int );
public:
	static constexpr auto PREFERRED_TRAVEL_FLAGS =
		TFL_WALK | TFL_WALKOFFLEDGE | TFL_JUMP | TFL_STRAFEJUMP | TFL_AIR | TFL_TELEPORT | TFL_JUMPPAD;
	static constexpr auto ALLOWED_TRAVEL_FLAGS =
		PREFERRED_TRAVEL_FLAGS | TFL_WATER | TFL_WATERJUMP | TFL_SWIM | TFL_LADDER | TFL_ELEVATOR | TFL_BARRIERJUMP;

	Bot( edict_t *self_, float skillLevel_ );

	~Bot() override;

	// For backward compatibility with dated code that should be rewritten
	const edict_t *Self() const { return self; }
	edict_t *Self() { return self; }

	// Should be preferred instead of use of Self() that is deprecated and will be removed
	int EntNum() const { return ENTNUM( self ); }

	int ClientNum() const { return ENTNUM( self ) - 1; }

	const player_state_t *PlayerState() const { return &self->r.client->ps; }
	player_state_t *PlayerState() { return &self->r.client->ps; }

	const float *Origin() const { return self->s.origin; }
	const float *Velocity() const { return self->velocity; }

	float Skill() const { return skillLevel; }
	bool IsReady() const { return level.ready[PLAYERNUM( self )]; }

	void OnPain( const edict_t *enemy, float kick, int damage ) {
		if( enemy != self ) {
			awarenessModule.OnPain( enemy, kick, damage );
		}
	}

	void OnKnockback( edict_t *attacker, const vec3_t basedir, int kick, int dflags ) {
		if( kick ) {
			lastKnockbackAt = level.time;
			VectorCopy( basedir, lastKnockbackBaseDir );
			if( attacker == self ) {
				lastOwnKnockbackKick = kick;
				lastOwnKnockbackAt = level.time;
			}
		}
	}

	void OnEnemyDamaged( const edict_t *enemy, int damage ) {
		if( enemy != self ) {
			awarenessModule.OnEnemyDamaged( enemy, damage );
		}
	}

	void OnEnemyOriginGuessed( const edict_t *enemy, unsigned millisSinceLastSeen, const float *guessedOrigin = nullptr ) {
		if( !guessedOrigin ) {
			guessedOrigin = enemy->s.origin;
		}
		awarenessModule.OnEnemyOriginGuessed( enemy, millisSinceLastSeen, guessedOrigin );
	}

	void RegisterEvent( const edict_t *ent, int event, int parm ) {
		awarenessModule.RegisterEvent( ent, event, parm );
	}

	void OnAttachedToSquad( AiSquad *squad_ ) {
		this->squad = squad_;
		awarenessModule.OnAttachedToSquad( squad_ );
		ForcePlanBuilding();
	}

	void OnDetachedFromSquad( AiSquad *squad_ ) {
		this->squad = nullptr;
		awarenessModule.OnDetachedFromSquad( squad_ );
		ForcePlanBuilding();
	}

	inline bool IsInSquad() const { return squad != nullptr; }

	/**
	 * Returns a timestamp of last attack (being hit) by an attacker.
	 * @note bots forget attack stats in a dozen of seconds.
	 * @param attacker an entity that (maybe) initiated an attack.
	 * @return a timestamp of last attack or 0 if a record of such event can't be found.
	 */
	int64_t LastAttackedByTime( const edict_t *attacker ) {
		return awarenessModule.LastAttackedByTime( attacker );
	}
	/**
	 * Returns a timestamp of last selection of a target by the bot.
	 * @note bots forget attack stats in a dozen of seconds.
	 * @param target an entity that (maybe) was selected as a target.
	 * @return a timestamp of last selection as a target or 0 if a record of such event can't be found.
	 */
	int64_t LastTargetTime( const edict_t *target ) {
		return awarenessModule.LastTargetTime( target );
	}

	void OnEnemyRemoved( const TrackedEnemy *enemy ) {
		awarenessModule.OnEnemyRemoved( enemy );
	}

	void OnHurtByNewThreat( const edict_t *newThreat, const AiFrameAwareUpdatable *threatDetector ) {
		awarenessModule.OnHurtByNewThreat( newThreat, threatDetector );
	}

	float GetBaseOffensiveness() const { return baseOffensiveness; }

	float GetEffectiveOffensiveness() const;

	void SetBaseOffensiveness( float baseOffensiveness_ ) {
		this->baseOffensiveness = baseOffensiveness_;
		clamp( this->baseOffensiveness, 0.0f, 1.0f );
	}

	void ClearOverriddenEntityWeights() {
		planningModule.ClearOverriddenEntityWeights();
	}

	void OverrideEntityWeight( const edict_t *ent, float weight ) {
		planningModule.OverrideEntityWeight( ent, weight );
	}

	const int *Inventory() const { return self->r.client->ps.inventory; }

	void EnableAutoAlert( const AiAlertSpot &alertSpot,
						  AlertTracker::AlertCallback callback,
						  AiFrameAwareUpdatable *receiver ) {
		awarenessModule.EnableAutoAlert( alertSpot, callback, receiver );
	}

	void DisableAutoAlert( int id ) {
		awarenessModule.DisableAutoAlert( id );
	}

	int Health() const {
		return self->r.client->ps.stats[STAT_HEALTH];
	}
	int Armor() const {
		return self->r.client->ps.stats[STAT_ARMOR];
	}

	bool CanAndWouldDropHealth() const {
		return GT_asBotWouldDropHealth( self->r.client );
	}

	void DropHealth() {
		GT_asBotDropHealth( self->r.client );
	}

	bool CanAndWouldDropArmor() const {
		return GT_asBotWouldDropArmor( self->r.client );
	}

	void DropArmor() {
		GT_asBotDropArmor( self->r.client );
	}

	float PlayerDefenciveAbilitiesRating() const {
		return GT_asPlayerDefenciveAbilitiesRating( self->r.client );
	}

	float PlayerOffenciveAbilitiesRating() const {
		return GT_asPlayerOffensiveAbilitiesRating( self->r.client );
	}

	const AiObjectiveSpot *ObjectiveSpot() const {
		return objectiveSpot;
	}

	void SetObjectiveSpot( AiObjectiveSpot *spot ) {
		objectiveSpot = spot;
	}

	int DefenceSpotId() const;
	int OffenseSpotId() const;

	/**
	 * Returns a field of view of the bot in degrees (dependent of skill level).
	 */
	float Fov() const { return 110.0f + 69.0f * Skill(); }
	/**
	 * Returns a value based on {@code Fov()} that is ready to be used
	 * in comparison of dot products of normalized vectors to determine visibility.
	 */
	float FovDotFactor() const { return cosf( (float)DEG2RAD( Fov() / 2 ) ); }

	BotBaseGoal *GetGoalByName( const char *name ) {
		return planningModule.GetGoalByName( name );
	}

	BotBaseAction *GetActionByName( const char *name ) {
		return planningModule.GetActionByName( name );
	}

	BotScriptGoal *InstantiateScriptGoal( void *scriptFactoryObject, const char *name, unsigned updatePeriod ) {
		return planningModule.InstantiateScriptGoal( scriptFactoryObject, name, updatePeriod );
	}

	BotScriptAction *InstantiateScriptAction( void *scriptFactoryObject, const char *name ) {
		return planningModule.InstantiateScriptAction( scriptFactoryObject, name );
	}

	const BotWeightConfig &WeightConfig() const { return weightConfig; }
	BotWeightConfig &WeightConfig() { return weightConfig; }

	void OnInterceptedPredictedEvent( int ev, int parm ) {
		movementModule.OnInterceptedPredictedEvent( ev, parm );
	}

	void OnInterceptedPMoveTouchTriggers( pmove_t *pm, const vec3_t previousOrigin ) {
		movementModule.OnInterceptedPMoveTouchTriggers( pm, previousOrigin );
	}

	const AiEntityPhysicsState *EntityPhysicsState() const {
		return entityPhysicsState;
	}

	// The movement code should use this method if there really are no
	// feasible ways to continue traveling to the nav target.
	void OnMovementToNavTargetBlocked();
protected:
	void Frame() override;
	void Think() override;

	void PreFrame() override {
		// We should update weapons status each frame since script weapons may be changed each frame.
		// These statuses are used by firing methods, so actual weapon statuses are required.
		weaponsUsageModule.UpdateScriptWeaponsStatus();
	}

	void SetFrameAffinity( unsigned modulo, unsigned offset ) override {
		AiFrameAwareUpdatable::SetFrameAffinity( modulo, offset );
		planningModule.SetFrameAffinity( modulo, offset );
		awarenessModule.SetFrameAffinity( modulo, offset );
	}

	void OnNavTargetTouchHandled() override {
		selectedNavEntity.InvalidateNextFrame();
	}

	void TouchedOtherEntity( const edict_t *entity ) override;
private:
	bool IsPrimaryAimEnemy( const edict_t *enemy ) const {
		return selectedEnemies.IsPrimaryEnemy( enemy );
	}

	// Put these often accessed members first

	AiSquad *squad;
	float skillLevel;
	float baseOffensiveness { 0.5f };

	unsigned similarWorldStateInstanceId { 0 };

	SelectedEnemies selectedEnemies;
	SelectedEnemies lostEnemies;
	SelectedMiscTactics selectedTactics;
	SelectedNavEntity selectedNavEntity;
	// For tracking picked up items
	const NavEntity *prevSelectedNavEntity { nullptr };

	// Put the movement module at the object beginning so the relative offset is small
	BotMovementModule movementModule;
	BotAwarenessModule awarenessModule;

	// Put planning module and weight config together
	BotPlanningModule planningModule;
	BotWeightConfig weightConfig;

	BotWeaponsUsageModule weaponsUsageModule;

	/**
	 * {@code next[]} and {@code prev[]} links below are addressed by these indices
	 */
	enum { SQUAD_LINKS, TMP_LINKS, TEAM_LINKS, OBJECTIVE_LINKS };

	Bot *next[4] { nullptr, nullptr, nullptr, nullptr };
	Bot *prev[4] { nullptr, nullptr, nullptr, nullptr };

	Bot *NextInSquad() { return next[SQUAD_LINKS]; };
	const Bot *NextInSquad() const { return next[SQUAD_LINKS]; }

	Bot *NextInTmpList() { return next[TMP_LINKS]; }
	const Bot *NextInTmpList() const { return next[TMP_LINKS]; }

	Bot *NextInBotsTeam() { return next[TEAM_LINKS]; }
	const Bot *NextInBotsTeam() const { return next[TEAM_LINKS]; }

	Bot *NextInObjective() { return next[OBJECTIVE_LINKS]; }
	const Bot *NextInObjective() const { return next[OBJECTIVE_LINKS]; }

	AiObjectiveSpot *objectiveSpot { nullptr };

	int64_t lastTouchedTeleportAt { 0 };
	int64_t lastTouchedJumppadAt { 0 };
	int64_t lastTouchedElevatorAt { 0 };
	int64_t lastKnockbackAt { 0 };
	int64_t lastOwnKnockbackAt { 0 };
	int lastOwnKnockbackKick { 0 };
	vec3_t lastKnockbackBaseDir;

	int64_t lastItemSelectedAt { 0 };
	int64_t noItemAvailableSince { 0 };

	int64_t lastBlockedNavTargetReportedAt { 0 };

	inline bool ShouldUseRoamSpotAsNavTarget() const {
		const auto &selectedNavEntity = GetSelectedNavEntity();
		// Wait for item selection in this case (the selection is just no longer valid).
		if( !selectedNavEntity.IsValid() ) {
			return false;
		}
		// There was a valid item selected
		if( !selectedNavEntity.IsEmpty() ) {
			return false;
		}

		return level.time - noItemAvailableSince > 3000;
	}

	// TODO: Move to the movement module
	class AiNavMeshQuery *navMeshQuery { nullptr };

	bool CanChangeWeapons() const {
		return movementModule.CanChangeWeapons();
	}

	void ChangeWeapons( const SelectedWeapons &selectedWeapons_ );
	void OnBlockedTimeout() override;
	void GhostingFrame();
	void ActiveFrame();
	void CallGhostingClientThink( const BotInput &input );
	void CallActiveClientThink( const BotInput &input );

	void OnRespawn();

	void CheckTargetProximity();

	bool HasJustPickedGoalItem() const;
public:
	// These methods are exposed mostly for script interface
	inline unsigned NextSimilarWorldStateInstanceId() {
		return ++similarWorldStateInstanceId;
	}

	int64_t LastTriggerTouchTime() const {
		return std::max( lastTouchedJumppadAt, std::max( lastTouchedTeleportAt, lastTouchedElevatorAt ) );
	}

	int64_t LastKnockbackAt() const { return lastKnockbackAt; }

	void ForceSetNavEntity( const SelectedNavEntity &selectedNavEntity_ );

	void ForcePlanBuilding() {
		basePlanner->ClearGoalAndPlan();
	}

	void SetCampingSpot( const AiCampingSpot &campingSpot ) {
		movementModule.SetCampingSpot( campingSpot );
	}
	void ResetCampingSpot() {
		movementModule.ResetCampingSpot();
	}
	bool HasActiveCampingSpot() const {
		return movementModule.HasActiveCampingSpot();
	}
	void SetPendingLookAtPoint( const AiPendingLookAtPoint &lookAtPoint, unsigned timeoutPeriod ) {
		return movementModule.SetPendingLookAtPoint( lookAtPoint, timeoutPeriod );
	}
	void ResetPendingLookAtPoint() {
		movementModule.ResetPendingLookAtPoint();
	}
	bool HasPendingLookAtPoint() const {
		return movementModule.HasPendingLookAtPoint();
	}

	bool CanInterruptMovement() const {
		return movementModule.CanInterruptMovement();
	}

	const SelectedNavEntity &GetSelectedNavEntity() const {
		return selectedNavEntity;
	}

	bool NavTargetWorthRushing() const;

	bool NavTargetWorthWeaponJumping() const {
		// TODO: Implement more sophisticated logic for this and another methods
		return NavTargetWorthRushing();
	}

	// Returns a number of weapons the logic allows to be used for weapon jumping.
	// The buffer is assumed to be capable to store all implemented weapons.
	int GetWeaponsForWeaponJumping( int *weaponNumsBuffer );

	const SelectedNavEntity &GetOrUpdateSelectedNavEntity();

	const SelectedEnemies &GetSelectedEnemies() const { return selectedEnemies; }

	const Hazard *PrimaryHazard() const {
		return awarenessModule.PrimaryHazard();
	}

	SelectedMiscTactics &GetMiscTactics() { return selectedTactics; }
	const SelectedMiscTactics &GetMiscTactics() const { return selectedTactics; }

	const AiAasRouteCache *RouteCache() const { return routeCache; }

	const TrackedEnemy *TrackedEnemiesHead() const {
		return awarenessModule.TrackedEnemiesHead();
	}

	const BotAwarenessModule::HurtEvent *ActiveHurtEvent() const {
		return awarenessModule.GetValidHurtEvent();
	}

	const float *GetKeptInFovPoint() const {
		return awarenessModule.GetKeptInFovPoint();
	}

	bool WillAdvance() const { return selectedTactics.willAdvance; }
	bool WillRetreat() const { return selectedTactics.willRetreat; }

	bool ShouldBeSilent() const { return selectedTactics.shouldBeSilent; }
	bool ShouldMoveCarefully() const { return selectedTactics.shouldMoveCarefully; }

	bool ShouldAttack() const { return selectedTactics.shouldAttack; }
	bool ShouldKeepXhairOnEnemy() const { return selectedTactics.shouldKeepXhairOnEnemy; }

	bool WillAttackMelee() const { return selectedTactics.willAttackMelee; }
	bool ShouldRushHeadless() const { return selectedTactics.shouldRushHeadless; }

	/**
	 * A hint for the weapon usage module.
	 * If true, bot should wait for better match of a "crosshair" and an enemy,
	 * otherwise shoot immediately if there is such opportunity.
	 */
	bool ShouldAimPrecisely() const {
		return ShouldKeepXhairOnEnemy() && planningModule.ShouldAimPrecisely();
	}

	// Whether the bot should stop bunnying even if it could produce
	// good predicted results and concentrate on combat/dodging
	bool ShouldSkinBunnyInFavorOfCombatMovement() const;
	// Whether it is allowed to dash right now
	bool IsCombatDashingAllowed() const;
	// Whether it is allowed to crouch right now
	bool IsCombatCrouchingAllowed() const;

	/**
	 * A wrapper over {@code AiManager::TryGetExpensiveComputationQuota()}.
	 * Should be used for situations where computations are not
	 * necessary for a bot lifecycle (but could improve behaviour).
	 */
	bool TryGetExtraComputationQuota() const;

	/**
	 * A wrapper over {@code AiManager::TryGetExpensiveComputationQuota()}.
	 * Should be used for situations where computations are mandatory
	 * for a bot lifecycle (e.g. a bot is blocked and has to suicide otherwise having no solution).
	 */
	bool TryGetVitalComputationQuota() const;

	/**
	 * A wrapper over {@code AiManager::TryGetExpensiveThinkCallQuota()}.
	 * Provided for consistency.
	 */
	bool TryGetExpensiveThinkCallQuota() const;
};

#endif
