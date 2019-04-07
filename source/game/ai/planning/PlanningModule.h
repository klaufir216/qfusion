#ifndef QFUSION_PLANNINGMODULE_H
#define QFUSION_PLANNINGMODULE_H

#include "Actions.h"
#include "BotPlanner.h"
#include "Goals.h"
#include "ItemsSelector.h"
#include "RoamingManager.h"
#include "TacticalSpotsCache.h"

class BotPlanningModule {
	friend class Bot;
	friend class BotPlanner;
	friend class BotAction;
	friend class BotGoal;
	friend class GrabItemGoal;
	friend class KillEnemyGoal;
	friend class RunAwayGoal;
	friend class ReactToHazardGoal;
	friend class ReactToThreatGoal;
	friend class ReactToEnemyLostGoal;
	friend class AttackOutOfDespairGoal;
	friend class RoamGoal;
	friend class BotTacticalSpotsCache;
	friend class WorldState;

	Bot *const bot;

	BotPlanner planner;

	GrabItemGoal grabItemGoal;
	KillEnemyGoal killEnemyGoal;
	RunAwayGoal runAwayGoal;
	ReactToHazardGoal reactToHazardGoal;
	ReactToThreatGoal reactToThreatGoal;
	ReactToEnemyLostGoal reactToEnemyLostGoal;
	AttackOutOfDespairGoal attackOutOfDespairGoal;
	RoamGoal roamGoal;

	RunToNavEntityAction runToNavEntityAction;
	PickupNavEntityAction pickupNavEntityAction;
	WaitForNavEntityAction waitForNavEntityAction;

	KillEnemyAction killEnemyAction;
	AdvanceToGoodPositionAction advanceToGoodPositionAction;
	RetreatToGoodPositionAction retreatToGoodPositionAction;
	GotoAvailableGoodPositionAction gotoAvailableGoodPositionAction;
	AttackFromCurrentPositionAction attackFromCurrentPositionAction;
	AttackAdvancingToTargetAction attackAdvancingToTargetAction;

	FleeToSpotAction fleeToSpotAction;
	StartGotoCoverAction startGotoCoverAction;
	TakeCoverAction takeCoverAction;

	StartGotoRunAwayTeleportAction startGotoRunAwayTeleportAction;
	DoRunAwayViaTeleportAction doRunAwayViaTeleportAction;
	StartGotoRunAwayJumppadAction startGotoRunAwayJumppadAction;
	DoRunAwayViaJumppadAction doRunAwayViaJumppadAction;
	StartGotoRunAwayElevatorAction startGotoRunAwayElevatorAction;
	DoRunAwayViaElevatorAction doRunAwayViaElevatorAction;
	StopRunningAwayAction stopRunningAwayAction;

	DodgeToSpotAction dodgeToSpotAction;

	TurnToThreatOriginAction turnToThreatOriginAction;

	TurnToLostEnemyAction turnToLostEnemyAction;
	StartLostEnemyPursuitAction startLostEnemyPursuitAction;
	StopLostEnemyPursuitAction stopLostEnemyPursuitAction;

	BotTacticalSpotsCache tacticalSpotsCache;
	BotItemsSelector itemsSelector;
	BotRoamingManager roamingManager;

	void RegisterBuiltinGoal( BotGoal &goal ) {
		planner.goals.push_back( &goal );
	}

	void RegisterBuiltinAction( BotAction &action ) {
		planner.actions.push_back( &action );
	}
public:
	explicit BotPlanningModule( Bot *bot_ );

	void RegisterBuiltinGoalsAndActions();

	BotGoal *GetGoalByName( const char *name ) { return planner.GetGoalByName( name ); }
	BotAction *GetActionByName( const char *name ) { return planner.GetActionByName( name ); }

	BotScriptGoal *InstantiateScriptGoal( void *scriptGoalsFactory, const char *name, unsigned updatePeriod );
	BotScriptAction *InstantiateScriptAction( void *scriptActionsFactory, const char *name );

	bool ShouldAimPrecisely() const {
		// Try shooting immediately if "attacking out of despair"
		return planner.activeGoal != &attackOutOfDespairGoal;
	}

	void ClearGoalAndPlan() { planner.ClearGoalAndPlan(); }

	const WorldState &CachedWorldState() const { return planner.cachedWorldState; }

	void CheckTargetProximity() { return roamingManager.CheckSpotsProximity(); }

	void OnMovementToNavEntityBlocked( const NavEntity *navEntity ) {
		roamingManager.DisableSpotsInRadius( navEntity->Origin(), 144.0f );
		itemsSelector.MarkAsDisabled( *navEntity, 4000 );
	}

	void ClearOverriddenEntityWeights() {
		itemsSelector.ClearOverriddenEntityWeights();
	}

	void OverrideEntityWeight( const edict_t *ent, float weight ) {
		itemsSelector.OverrideEntityWeight( ent, weight );
	}

	SelectedNavEntity SuggestGoalNavEntity( const SelectedNavEntity &currSelectedNavEntity ) {
		return itemsSelector.SuggestGoalNavEntity( currSelectedNavEntity );
	}

	bool IsTopTierItem( const NavTarget *navTarget ) const {
		return itemsSelector.IsTopTierItem( navTarget );
	}

	void SetFrameAffinity( unsigned modulo, unsigned offset ) {
		planner.SetFrameAffinity( modulo, offset );
	}

	const ArrayRange<AiGoal *> Goals() const {
		return ArrayRange<AiGoal *>( planner.goals.begin(), planner.goals.size() );
	}

	const ArrayRange<AiAction *> Actions() const {
		return ArrayRange<AiAction *>( planner.actions.begin(), planner.actions.size() );
	}
};

#endif
