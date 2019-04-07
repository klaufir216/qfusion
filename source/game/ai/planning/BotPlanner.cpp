#include "../bot.h"
#include "../ai_ground_trace_cache.h"
#include "../teamplay/SquadBasedTeam.h"
#include "BotPlanner.h"
#include "PlanningLocal.h"
#include "../combat/DodgeHazardProblemSolver.h"
#include <algorithm>
#include <limits>
#include <stdarg.h>

BotPlanner::BotPlanner( Bot *bot_, BotPlanningModule *module_ )
	: AiPlanner( bot_, &module_->goals, &module_->actions )
	, bot( bot_ ), module( module_ ), cachedWorldState( bot_ ) {}

const int *BotPlanner::Inventory() const {
	return game.edicts[bot->EntNum()].r.client->ps.inventory;
}

BotGoal *BotPlanner::GetGoalByName( const char *name ) {
	for( auto &goal: scriptGoals ) {
		if( !Q_stricmp( name, goal.Name() ) ) {
			return &goal;
		}
	}

	return nullptr;
}

BotAction *BotPlanner::GetActionByName( const char *name ) {
	for( auto &action: scriptActions ) {
		if( !Q_stricmp( name, action.Name() ) ) {
			return &action;
		}
	}

	return nullptr;
}

bool BotPlanner::FindDodgeHazardSpot( const Hazard &hazard, vec3_t spotOrigin ) {
	float radius = 128.0f + 192.0f * bot->Skill();
	typedef DodgeHazardProblemSolver Solver;
	Solver::OriginParams originParams( game.edicts + bot->EntNum(), radius, bot->RouteCache() );
	Solver::ProblemParams problemParams( hazard.hitPoint, hazard.direction, hazard.IsSplashLike() );
	problemParams.SetCheckToAndBackReach( false );
	problemParams.SetMinHeightAdvantageOverOrigin( -64.0f );
	// Influence values are quite low because evade direction factor must be primary
	problemParams.SetHeightOverOriginInfluence( 0.2f );
	problemParams.SetMaxFeasibleTravelTimeMillis( 2500 );
	problemParams.SetOriginDistanceInfluence( 0.4f );
	problemParams.SetOriginWeightFalloffDistanceRatio( 0.9f );
	problemParams.SetTravelTimeInfluence( 0.2f );
	return Solver( originParams, problemParams ).FindSingle( spotOrigin );
}

void BotPlanner::PrepareCurrWorldState( WorldState *worldState ) {
	worldState->SetIgnoreAll( false );

	worldState->BotOriginVar().SetValue( bot->Origin() );
	worldState->PendingOriginVar().SetIgnore( true );

	const auto &selectedEnemies = bot->GetSelectedEnemies();

	if( selectedEnemies.AreValid() ) {
		worldState->EnemyOriginVar().SetValue( selectedEnemies.LastSeenOrigin() );
		worldState->HasThreateningEnemyVar().SetValue( selectedEnemies.AreThreatening() );
		worldState->RawDamageToKillVar().SetValue( (short)selectedEnemies.DamageToKill() );
		worldState->EnemyHasQuadVar().SetValue( selectedEnemies.HaveQuad() );
		worldState->EnemyHasGoodSniperRangeWeaponsVar().SetValue( selectedEnemies.HaveGoodSniperRangeWeapons() );
		worldState->EnemyHasGoodFarRangeWeaponsVar().SetValue( selectedEnemies.HaveGoodFarRangeWeapons() );
		worldState->EnemyHasGoodMiddleRangeWeaponsVar().SetValue( selectedEnemies.HaveGoodMiddleRangeWeapons() );
		worldState->EnemyHasGoodCloseRangeWeaponsVar().SetValue( selectedEnemies.HaveGoodCloseRangeWeapons() );
		worldState->CanHitEnemyVar().SetValue( selectedEnemies.CanBeHit() );
	} else {
		worldState->EnemyOriginVar().SetIgnore( true );
		worldState->HasThreateningEnemyVar().SetIgnore( true );
		worldState->RawDamageToKillVar().SetIgnore( true );
		worldState->EnemyHasQuadVar().SetIgnore( true );
		worldState->EnemyHasGoodSniperRangeWeaponsVar().SetIgnore( true );
		worldState->EnemyHasGoodFarRangeWeaponsVar().SetIgnore( true );
		worldState->EnemyHasGoodMiddleRangeWeaponsVar().SetIgnore( true );
		worldState->EnemyHasGoodCloseRangeWeaponsVar().SetIgnore( true );
		worldState->EnemyCanHitVar().SetIgnore( true );
		worldState->CanHitEnemyVar().SetIgnore( true );
	}

	auto &lostEnemies = bot->lostEnemies;
	if( lostEnemies.AreValid() ) {
		worldState->IsReactingToEnemyLostVar().SetValue( false );
		worldState->HasReactedToEnemyLostVar().SetValue( false );
		worldState->LostEnemyLastSeenOriginVar().SetValue( lostEnemies.LastSeenOrigin() );
		worldState->MightSeeLostEnemyAfterTurnVar().SetValue( false );
		Vec3 toEnemiesDir( lostEnemies.LastSeenOrigin() );
		toEnemiesDir -= bot->Origin();
		toEnemiesDir.NormalizeFast();
		if( toEnemiesDir.Dot( bot->EntityPhysicsState()->ForwardDir() ) < bot->FovDotFactor() ) {
			edict_t *self = game.edicts + bot->EntNum();
			if( EntitiesPvsCache::Instance()->AreInPvs( self, lostEnemies.TraceKey() ) ) {
				trace_t trace;
				G_Trace( &trace, self->s.origin, nullptr, nullptr, lostEnemies.LastSeenOrigin().Data(), self, MASK_AISOLID );
				if( trace.fraction == 1.0f || game.edicts + trace.ent == lostEnemies.TraceKey() ) {
					worldState->MightSeeLostEnemyAfterTurnVar().SetValue( true );
				}
			}
		}
	} else {
		worldState->IsReactingToEnemyLostVar().SetIgnore( true );
		worldState->HasReactedToEnemyLostVar().SetIgnore( true );
		worldState->LostEnemyLastSeenOriginVar().SetIgnore( true );
		worldState->MightSeeLostEnemyAfterTurnVar().SetIgnore( true );
	}

	const edict_t *self = game.edicts + bot->EntNum();
	worldState->HealthVar().SetValue( (short)HEALTH_TO_INT( self->health ) );
	worldState->ArmorVar().SetValue( self->r.client->ps.stats[STAT_ARMOR] );

	worldState->HasQuadVar().SetValue( ::HasQuad( self ) );
	worldState->HasShellVar().SetValue( ::HasShell( self ) );

	bool hasGoodSniperRangeWeapons = false;
	bool hasGoodFarRangeWeapons = false;
	bool hasGoodMiddleRangeWeapons = false;
	bool hasGoodCloseRangeWeapons = false;

	if( BoltsReadyToFireCount() || BulletsReadyToFireCount() || InstasReadyToFireCount() ) {
		hasGoodSniperRangeWeapons = true;
	}
	if( BoltsReadyToFireCount() || BulletsReadyToFireCount() || PlasmasReadyToFireCount() || InstasReadyToFireCount() ) {
		hasGoodFarRangeWeapons = true;
	}
	if( RocketsReadyToFireCount() || LasersReadyToFireCount() || PlasmasReadyToFireCount() ||
		BulletsReadyToFireCount() || ShellsReadyToFireCount() || InstasReadyToFireCount() || WavesReadyToFireCount() ) {
		hasGoodMiddleRangeWeapons = true;
	}
	if( RocketsReadyToFireCount() || PlasmasReadyToFireCount() || ShellsReadyToFireCount() || WavesReadyToFireCount() ) {
		hasGoodCloseRangeWeapons = true;
	}

	worldState->HasGoodSniperRangeWeaponsVar().SetValue( hasGoodSniperRangeWeapons );
	worldState->HasGoodFarRangeWeaponsVar().SetValue( hasGoodFarRangeWeapons );
	worldState->HasGoodMiddleRangeWeaponsVar().SetValue( hasGoodMiddleRangeWeapons );
	worldState->HasGoodCloseRangeWeaponsVar().SetValue( hasGoodCloseRangeWeapons );

	worldState->HasQuadVar().SetValue( ::HasQuad( self ) );
	worldState->HasShellVar().SetValue( ::HasShell( self ) );

	const SelectedNavEntity &currSelectedNavEntity = bot->GetOrUpdateSelectedNavEntity();
	if( currSelectedNavEntity.IsEmpty() ) {
		// HACK! If there is no selected nav entity, set the value to the roaming spot origin.
		if( bot->ShouldUseRoamSpotAsNavTarget() ) {
			Vec3 spot( module->roamingManager.GetCachedRoamingSpot() );
			Debug( "Using a roaming spot @ %.1f %.1f %.1f as a world state nav target var\n", spot.X(), spot.Y(), spot.Z() );
			worldState->NavTargetOriginVar().SetValue( spot );
		} else {
			worldState->NavTargetOriginVar().SetIgnore( true );
		}
		worldState->GoalItemWaitTimeVar().SetIgnore( true );
	} else {
		const NavEntity *navEntity = currSelectedNavEntity.GetNavEntity();
		worldState->NavTargetOriginVar().SetValue( navEntity->Origin() );
		// Find a travel time to the goal itme nav entity in milliseconds
		// We hope this router call gets cached by AAS subsystem
		int areaNums[2] = { 0, 0 };
		int numAreas = bot->EntityPhysicsState()->PrepareRoutingStartAreas( areaNums );
		const auto *routeCache = bot->RouteCache();
		unsigned travelTime = 10U * routeCache->PreferredRouteToGoalArea( areaNums, numAreas, navEntity->AasAreaNum() );
		// AAS returns 1 seconds^-2 as a lowest feasible value
		if( travelTime <= 10 ) {
			travelTime = 0;
		}
		int64_t spawnTime = navEntity->SpawnTime();
		// If the goal item spawns before the moment when it gets reached
		if( level.time + travelTime >= spawnTime ) {
			worldState->GoalItemWaitTimeVar().SetValue( 0 );
		} else {
			worldState->GoalItemWaitTimeVar().SetValue( (unsigned)( spawnTime - level.time - travelTime ) );
		}
	}

	worldState->HasJustPickedGoalItemVar().SetValue( bot->HasJustPickedGoalItem() );

	worldState->HasPositionalAdvantageVar().SetValue( false );

	worldState->HasJustKilledEnemyVar().SetValue( false );

	// If methods corresponding to these comparisons are extracted, their names will be confusing
	// (they are useful for filling world state only as not always corresponding to what a human caller expect).
	worldState->HasJustTeleportedVar().SetValue( level.time - bot->lastTouchedTeleportAt < 64 + 1 );
	worldState->HasJustTouchedJumppadVar().SetValue( level.time - bot->lastTouchedJumppadAt < 64 + 1 );
	worldState->HasJustEnteredElevatorVar().SetValue( level.time - bot->lastTouchedElevatorAt < 64 + 1 );

	worldState->HasPendingCoverSpotVar().SetIgnore( true );
	worldState->HasPendingRunAwayTeleportVar().SetIgnore( true );
	worldState->HasPendingRunAwayJumppadVar().SetIgnore( true );
	worldState->HasPendingRunAwayElevatorVar().SetIgnore( true );

	worldState->IsRunningAwayVar().SetIgnore( true );
	worldState->HasRunAwayVar().SetIgnore( true );

	const Hazard *activeHazard = bot->PrimaryHazard();
	worldState->HasReactedToHazardVar().SetValue( false );
	if( bot->Skill() > 0.33f && activeHazard ) {
		worldState->PotentialHazardDamageVar().SetValue( (short)activeHazard->damage );
		worldState->HazardHitPointVar().SetValue( activeHazard->hitPoint );
		worldState->HazardDirectionVar().SetValue( activeHazard->direction );
		vec3_t dodgeHazardSpot;
		if( FindDodgeHazardSpot( *activeHazard, dodgeHazardSpot ) ) {
			worldState->DodgeHazardSpotVar().SetValue( dodgeHazardSpot );
		} else {
			worldState->DodgeHazardSpotVar().SetIgnore( true );
		}
	} else {
		worldState->PotentialHazardDamageVar().SetIgnore( true );
		worldState->HazardHitPointVar().SetIgnore( true );
		worldState->HazardDirectionVar().SetIgnore( true );
		worldState->DodgeHazardSpotVar().SetIgnore( true );
	}

	if( const auto *activeThreat = bot->ActiveHurtEvent() ) {
		worldState->ThreatInflictedDamageVar().SetValue( (short)activeThreat->totalDamage );
		worldState->ThreatPossibleOriginVar().SetValue( activeThreat->possibleOrigin );
		worldState->HasReactedToThreatVar().SetValue( false );
	} else {
		worldState->ThreatInflictedDamageVar().SetIgnore( true );
		worldState->ThreatPossibleOriginVar().SetIgnore( true );
		worldState->HasReactedToThreatVar().SetIgnore( true );
	}

	worldState->ResetTacticalSpots();

	worldState->SimilarWorldStateInstanceIdVar().SetIgnore( true );

	cachedWorldState = *worldState;
}

// Cannot be defined in the header
bool BotPlanner::ShouldSkipPlanning() const {
	return !bot->CanInterruptMovement();
}

void BotPlanner::BeforePlanning() {
	AiPlanner::BeforePlanning();

	module->tacticalSpotsCache.Clear();
}

float Bot::GetEffectiveOffensiveness() const {
	if( squad ) {
		return squad->IsSupporter( self ) ? 1.0f : 0.0f;
	}
	if( selectedEnemies.AreValid() && selectedEnemies.HaveCarrier() ) {
		return 0.75f;
	}
	return baseOffensiveness;
}
