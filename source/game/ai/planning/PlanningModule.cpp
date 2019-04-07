#include "PlanningModule.h"
#include "PlanningLocal.h"

BotPlanningModule::BotPlanningModule( Bot *bot_ )
	: bot( bot_ )
	, planner( bot_, this )
	, grabItemGoal( this )
	, killEnemyGoal( this )
	, runAwayGoal( this )
	, reactToHazardGoal( this )
	, reactToThreatGoal( this )
	, reactToEnemyLostGoal( this )
	, attackOutOfDespairGoal( this )
	, roamGoal( this )
	, runToNavEntityAction( this )
	, pickupNavEntityAction( this )
	, waitForNavEntityAction( this )
	, killEnemyAction( this )
	, advanceToGoodPositionAction( this )
	, retreatToGoodPositionAction( this )
	, gotoAvailableGoodPositionAction( this )
	, attackFromCurrentPositionAction( this )
	, attackAdvancingToTargetAction( this )
	, fleeToSpotAction( this )
	, startGotoCoverAction( this )
	, takeCoverAction( this )
	, startGotoRunAwayTeleportAction( this )
	, doRunAwayViaTeleportAction( this )
	, startGotoRunAwayJumppadAction( this )
	, doRunAwayViaJumppadAction( this )
	, startGotoRunAwayElevatorAction( this )
	, doRunAwayViaElevatorAction( this )
	, stopRunningAwayAction( this )
	, dodgeToSpotAction( this )
	, turnToThreatOriginAction( this )
	, turnToLostEnemyAction( this )
	, startLostEnemyPursuitAction( this )
	, stopLostEnemyPursuitAction( this )
	, tacticalSpotsCache( bot_ )
	, itemsSelector( bot_ )
	, roamingManager( bot_ ) {}

void BotPlanningModule::RegisterBuiltinGoalsAndActions() {
	RegisterBuiltinGoal( grabItemGoal );
	RegisterBuiltinGoal( killEnemyGoal );
	RegisterBuiltinGoal( runAwayGoal );
	RegisterBuiltinGoal( reactToHazardGoal );
	RegisterBuiltinGoal( reactToThreatGoal );
	RegisterBuiltinGoal( reactToEnemyLostGoal );
	RegisterBuiltinGoal( attackOutOfDespairGoal );
	RegisterBuiltinGoal( roamGoal );

	RegisterBuiltinAction( runToNavEntityAction );
	RegisterBuiltinAction( pickupNavEntityAction );
	RegisterBuiltinAction( waitForNavEntityAction );

	RegisterBuiltinAction( killEnemyAction );
	RegisterBuiltinAction( advanceToGoodPositionAction );
	RegisterBuiltinAction( retreatToGoodPositionAction );
	RegisterBuiltinAction( gotoAvailableGoodPositionAction );
	RegisterBuiltinAction( attackFromCurrentPositionAction );
	RegisterBuiltinAction( attackAdvancingToTargetAction );

	RegisterBuiltinAction( fleeToSpotAction );
	RegisterBuiltinAction( startGotoCoverAction );
	RegisterBuiltinAction( takeCoverAction );

	RegisterBuiltinAction( startGotoRunAwayTeleportAction );
	RegisterBuiltinAction( doRunAwayViaTeleportAction );
	RegisterBuiltinAction( startGotoRunAwayJumppadAction );
	RegisterBuiltinAction( doRunAwayViaJumppadAction );
	RegisterBuiltinAction( startGotoRunAwayElevatorAction );
	RegisterBuiltinAction( doRunAwayViaElevatorAction );
	RegisterBuiltinAction( stopRunningAwayAction );

	RegisterBuiltinAction( dodgeToSpotAction );

	RegisterBuiltinAction( turnToThreatOriginAction );

	RegisterBuiltinAction( turnToLostEnemyAction );
	RegisterBuiltinAction( startLostEnemyPursuitAction );
	RegisterBuiltinAction( stopLostEnemyPursuitAction );
}


BotScriptGoal *BotPlanningModule::InstantiateScriptGoal( void *scriptGoalFactory,
														 const char *name,
														 unsigned updatePeriod ) {
	// We have to use a 2-phase construction.
	// GENERIC_asInstantiateGoal() expects a persistent memory address for a native object reference.
	// BotScriptGoal constructor expects a persistent script object address too.
	// We defer BotScriptGoal constructor call to break this loop.
	// GENERIC_asInstantiateGoal() script counterpart must be aware that the native object is not constructed yet.
	BotScriptGoal *nativeAddress = planner.AllocScriptGoal();
	void *scriptObject = GENERIC_asInstantiateGoal( scriptGoalFactory, game.edicts + bot->EntNum(), nativeAddress );
	return new( nativeAddress )BotScriptGoal( this, name, updatePeriod, scriptObject );
}

BotScriptAction *BotPlanningModule::InstantiateScriptAction( void *scriptActionFactory, const char *name ) {
	// See the explanation above related to a script goal, this is a similar case
	BotScriptAction *nativeAddress = planner.AllocScriptAction();
	void *scriptObject = GENERIC_asInstantiateAction( scriptActionFactory, game.edicts + bot->EntNum(), nativeAddress );
	return new( nativeAddress )BotScriptAction( this, name, scriptObject );
}