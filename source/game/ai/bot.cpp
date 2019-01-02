#include "bot.h"
#include "navigation/AasWorld.h"
#include "navigation/NavMeshManager.h"
#include "teamplay/ObjectiveBasedTeam.h"
#include "../g_gametypes.h"
#include "ai_manager.h"
#include <algorithm>
#include <array>

#ifndef _MSC_VER
// Allow getting an address of not initialized yet field movementModule.movementState.entityPhysicsState.
// Saving this address for further use is legal, the field is not going to be used right now.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
#endif

Bot::Bot( edict_t *self_, float skillLevel_ )
	: Ai( self_
		, &planningModule.planner
		, AiAasRouteCache::NewInstance( &travelFlags[0] )
		, &movementModule.movementState.entityPhysicsState
		, PREFERRED_TRAVEL_FLAGS
		, ALLOWED_TRAVEL_FLAGS
		, skillLevel_ > 0.33f ? DEFAULT_YAW_SPEED * 1.5f : DEFAULT_YAW_SPEED
		, skillLevel_ > 0.33f ? DEFAULT_PITCH_SPEED * 1.2f : DEFAULT_PITCH_SPEED )
	, skillLevel( skillLevel_ )
	, selectedEnemies( self_ )
	, lostEnemies( self_ )
	, selectedNavEntity( nullptr, 0, 0, 0 )
	, movementModule( this )
	, awarenessModule( self_, this, skillLevel_ )
	, planningModule( self_, this, skillLevel_ )
	, weightConfig( self_ )
	, weaponsUsageModule( this ) {
	self->r.client->movestyle = GS_CLASSICBUNNY;
	// Enable skimming for bots (since it is useful and should not be noticed from a 3rd person POV).
	self->r.client->ps.pmove.stats[PM_STAT_FEATURES] &= PMFEAT_CORNERSKIMMING;
	SetTag( self->r.client->netname );
}

#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif

Bot::~Bot() {
	AiAasRouteCache::ReleaseInstance( routeCache );

	if( navMeshQuery ) {
		AiNavMeshManager::Instance()->FreeQuery( navMeshQuery );
	}
}

int Bot::DefenceSpotId() const {
	// This call is used only for scripts compatibility so this is not that bad
	if( dynamic_cast<AiDefenceSpot *>( objectiveSpot ) ) {
		return objectiveSpot->id;
	}
	return -1;
}

int Bot::OffenseSpotId() const {
	// This call is used only for scripts compatibility so this is not that bad
	if( dynamic_cast<AiOffenseSpot *>( objectiveSpot ) ) {
		return objectiveSpot->id;
	}
	return -1;
}

void Bot::TouchedOtherEntity( const edict_t *entity ) {
	if( !entity->classname ) {
		return;
	}

	// Cut off string comparisons by doing these cheap tests first

	// Only triggers are interesting for following code
	if( entity->r.solid != SOLID_TRIGGER ) {
		return;
	}
	// Items should be handled by TouchedNavEntity() or skipped (if it is not a current nav entity)
	if( entity->item ) {
		return;
	}

	if( !Q_stricmp( entity->classname, "trigger_push" ) ) {
		lastTouchedJumppadAt = level.time;
		movementModule.ActivateJumppadState( entity );
		return;
	}

	if( !Q_stricmp( entity->classname, "trigger_teleport" ) ) {
		lastTouchedTeleportAt = level.time;
		return;
	}

	if( !Q_stricmp( entity->classname, "func_plat" ) ) {
		lastTouchedElevatorAt = level.time;
		return;
	}
}

bool Bot::HasJustPickedGoalItem() const {
	if( lastNavTargetReachedAt < prevThinkAt ) {
		return false;
	}
	if( !lastReachedNavTarget ) {
		return false;
	}
	if( !lastReachedNavTarget->IsBasedOnNavEntity( prevSelectedNavEntity ) ) {
		return false;
	}
	return true;
}

void Bot::CheckTargetProximity() {
	planningModule.CheckTargetProximity();

	if( !NavTargetAasAreaNum() ) {
		return;
	}

	if( !IsCloseToNavTarget( 128.0f ) ) {
		return;
	}

	// Save the origin for the roaming manager to avoid its occasional modification in the code below
	if( !TryReachNavTargetByProximity() ) {
		return;
	}

	OnNavTargetTouchHandled();
}

const SelectedNavEntity &Bot::GetOrUpdateSelectedNavEntity() {
	if( selectedNavEntity.IsValid() && !selectedNavEntity.IsEmpty() ) {
		return selectedNavEntity;
	}

	// Force an update using the currently selected nav entity
	// (it's OK if it's not valid) as a reference info for selection
	ForceSetNavEntity( planningModule.SuggestGoalNavEntity( selectedNavEntity ) );
	// Return the modified selected nav entity
	return selectedNavEntity;
}

void Bot::ForceSetNavEntity( const SelectedNavEntity &selectedNavEntity_ ) {
	// Use direct access to the field to skip assertion
	this->prevSelectedNavEntity = this->selectedNavEntity.navEntity;
	this->selectedNavEntity = selectedNavEntity_;

	if( !selectedNavEntity.IsEmpty() ) {
		self->ai->botRef->lastItemSelectedAt = level.time;
	} else if( self->ai->botRef->lastItemSelectedAt >= self->ai->botRef->noItemAvailableSince ) {
		self->ai->botRef->noItemAvailableSince = level.time;
	}
}

void Bot::ChangeWeapons( const SelectedWeapons &selectedWeapons_ ) {
	if( selectedWeapons_.BuiltinFireDef() != nullptr ) {
		self->r.client->ps.stats[STAT_PENDING_WEAPON] = selectedWeapons_.BuiltinWeaponNum();
	}
	if( selectedWeapons_.ScriptFireDef() != nullptr ) {
		GT_asSelectScriptWeapon( self->r.client, selectedWeapons_.ScriptWeaponNum() );
	}
}

//==========================================
// BOT_DMClass_BlockedTimeout
// the bot has been blocked for too long
//==========================================
void Bot::OnBlockedTimeout() {
	self->health = 0;
	blockedTimeoutAt = level.time + BLOCKED_TIMEOUT;
	self->die( self, self, self, 100000, vec3_origin );
	G_Killed( self, self, self, 999, vec3_origin, MOD_SUICIDE );
	self->nextThink = level.time + 1;
}

//==========================================
// BOT_DMclass_DeadFrame
// ent is dead = run this think func
//==========================================
void Bot::GhostingFrame() {
	selectedEnemies.Invalidate();

	planningModule.ClearGoalAndPlan();

	movementModule.Reset();

	blockedTimeoutAt = level.time + BLOCKED_TIMEOUT;
	self->nextThink = level.time + 100;

	// Release this quite huge object while it is not really needed.
	// We have decided avoid its preallocation and ignore allocation failures.
	if( navMeshQuery ) {
		AiNavMeshManager::Instance()->FreeQuery( navMeshQuery );
		navMeshQuery = nullptr;
	}

	// wait 4 seconds after entering the level
	if( self->r.client->level.timeStamp + 4000 > level.time || !level.canSpawnEntities ) {
		return;
	}

	if( self->r.client->team == TEAM_SPECTATOR ) {
		// try to join a team
		// note that G_Teams_JoinAnyTeam is quite slow so only call it per frame
		if( !self->r.client->queueTimeStamp && self == level.think_client_entity ) {
			G_Teams_JoinAnyTeam( self, false );
		}

		if( self->r.client->team == TEAM_SPECTATOR ) { // couldn't join, delay the next think
			self->nextThink = level.time + 2000 + (int)( 4000 * random() );
		} else {
			self->nextThink = level.time + 1;
		}
		return;
	}

	BotInput botInput;
	botInput.isUcmdSet = true;
	// ask for respawn if the minimum bot respawning time passed
	if( level.time > self->deathTimeStamp + 3000 ) {
		botInput.SetAttackButton( true );
	}

	CallGhostingClientThink( botInput );
}

void Bot::CallGhostingClientThink( const BotInput &input ) {
	usercmd_t ucmd;
	// Shut an analyzer up
	memset( &ucmd, 0, sizeof( usercmd_t ) );
	input.CopyToUcmd( &ucmd );
	// set approximate ping and show values
	ucmd.serverTimeStamp = game.serverTime;
	ucmd.msec = (uint8_t)game.frametime;
	self->r.client->r.ping = 0;

	ClientThink( self, &ucmd, 0 );
}

void Bot::OnRespawn() {
	VectorClear( self->r.client->ps.pmove.delta_angles );
	self->r.client->level.last_activity = level.time;

	ResetNavigation();
}

void Bot::Think() {
	// Call superclass method first
	Ai::Think();

	if( IsGhosting() ) {
		return;
	}

	// TODO: Let the weapons usage module decide?
	if( CanChangeWeapons() ) {
		weaponsUsageModule.Think( planningModule.CachedWorldState() );
		ChangeWeapons( weaponsUsageModule.GetSelectedWeapons() );
	}
}

//==========================================
// BOT_DMclass_RunFrame
// States Machine & call client movement
//==========================================
void Bot::Frame() {
	// Call superclass method first
	Ai::Frame();

	if( IsGhosting() ) {
		GhostingFrame();
	} else {
		ActiveFrame();
	}
}

void Bot::ActiveFrame() {
	//get ready if in the game
	if( GS_MatchState() <= MATCH_STATE_WARMUP && !IsReady() && self->r.client->teamstate.timeStamp + 4000 < level.time ) {
		G_Match_Ready( self );
	}

	// Always calls Frame() and calls Think() if needed
	awarenessModule.Update();

	weaponsUsageModule.Frame( planningModule.CachedWorldState() );

	BotInput botInput;
	// Might modify botInput
	movementModule.Frame( &botInput );

	CheckTargetProximity();

	// Might modify botInput
	if( ShouldAttack() ) {
		weaponsUsageModule.TryFire( &botInput );
	}

	// Apply modified botInput
	movementModule.ApplyInput( &botInput );
	CallActiveClientThink( botInput );
}

void Bot::CallActiveClientThink( const BotInput &input ) {
	usercmd_t ucmd;
	// Shut an analyzer up
	memset( &ucmd, 0, sizeof( usercmd_t ) );
	input.CopyToUcmd( &ucmd );

	//set up for pmove
	for( int i = 0; i < 3; i++ )
		ucmd.angles[i] = (short)ANGLE2SHORT( self->s.angles[i] ) - self->r.client->ps.pmove.delta_angles[i];

	VectorSet( self->r.client->ps.pmove.delta_angles, 0, 0, 0 );

	// set approximate ping and show values
	ucmd.msec = (uint8_t)game.frametime;
	ucmd.serverTimeStamp = game.serverTime;

	ClientThink( self, &ucmd, 0 );
	self->nextThink = level.time + 1;
}

void Bot::OnMovementToNavTargetBlocked() {
	if( !selectedNavEntity.IsValid() || selectedNavEntity.IsEmpty() ) {
		return;
	}

	// If a new nav target is set in blocked state, the bot remains blocked
	// for few millis since the ground acceleration is finite.
	// Prevent classifying just set nav targets as ones that have led to blocking.
	if( level.time - lastBlockedNavTargetReportedAt < 400 ) {
		return;
	}

	lastBlockedNavTargetReportedAt = level.time;

	// Force replanning
	planningModule.ClearGoalAndPlan();

	const auto *navEntity = selectedNavEntity.GetNavEntity();
	if( navEntity ) {
		planningModule.OnMovementToNavEntityBlocked( navEntity );
		selectedNavEntity.InvalidateNextFrame();
		return;
	}

	selectedNavEntity.InvalidateNextFrame();
}

bool Bot::NavTargetWorthRushing() const {
	if( ShouldBeSilent() || ShouldMoveCarefully() ) {
		return false;
	}

	if( ShouldRushHeadless() ) {
		return true;
	}

	if( !GS_SelfDamage() ) {
		return true;
	}

	// If the bot cannot refill health
	if( !( level.gametype.spawnableItemsMask & IT_HEALTH ) ) {
		// TODO: Allow it at the end of round. How to detect a round state in the native code?
		return false;
	}

	return selectedEnemies.AreValid() && planningModule.IsTopTierItem( navTarget );
}

int Bot::GetWeaponsForWeaponJumping( int *weaponNumsBuffer ) {
	// TODO: Implement more sophisticated logic
	if( ShouldBeSilent() || ShouldMoveCarefully() ) {
		return 0;
	}

	int numSuitableWeapons = 0;
	const auto *inventory = self->r.client->ps.inventory;

	if( g_instajump->integer ) {
		if( inventory[WEAP_INSTAGUN] && inventory[AMMO_INSTAS] ) {
			weaponNumsBuffer[numSuitableWeapons++] = WEAP_INSTAGUN;
		}
	}

	// We have decided to avoid using Shockwave...
	std::array<int, 2> rlPriorityWeapons = { { WEAP_ROCKETLAUNCHER, WEAP_GUNBLADE } };
	std::array<int, 2> gbPriorityWeapons = { { WEAP_GUNBLADE, WEAP_ROCKETLAUNCHER } };
	const std::array<int, 2> *weaponsList;

	if( g_allow_selfdamage->integer ) {
		weaponsList = &gbPriorityWeapons;
		float damageToKill = DamageToKill( self, g_armor_protection->value, g_armor_degradation->value );
		if( HasQuad( self ) ) {
			damageToKill /= 4.0f;
		}
		if( HasShell( self ) ) {
			damageToKill *= 4.0f;
			weaponsList = &rlPriorityWeapons;
		}

		for( int weapon: *weaponsList ) {
			if( inventory[weapon] && inventory[AMMO_GUNBLADE + ( weapon - WEAP_GUNBLADE )] ) {
				const auto &firedef = GS_GetWeaponDef( weapon )->firedef;
				if( firedef.damage * firedef.selfdamage + 15 < damageToKill ) {
					weaponNumsBuffer[numSuitableWeapons++] = weapon;
				}
			}
		}
	} else {
		// Prefer RL as it is very likely to be the CA gametype and high knockback is expected
		weaponsList = &rlPriorityWeapons;
		if( inventory[AMMO_ROCKETS] < 5 ) {
			// Save RL ammo in this case
			weaponsList = &gbPriorityWeapons;
		}
		for( int weapon: *weaponsList ) {
			if( inventory[weapon] && inventory[AMMO_GUNBLADE + ( weapon - WEAP_GUNBLADE )] ) {
				weaponNumsBuffer[numSuitableWeapons++] = weapon;
			}
		}
	}

	return numSuitableWeapons;
}

bool Bot::ShouldSkinBunnyInFavorOfCombatMovement() const {
	// Return a feasible value for this case
	if( !selectedEnemies.AreValid() ) {
		return false;
	}

	// Self-descriptive...
	if( ShouldRushHeadless() ) {
		return false;
	}

	// Prepare to avoid/dodge an EB/IG shot
	if( selectedEnemies.AreAboutToHitEBorIG() ) {
		return true;
	}

	// Prepare to avoid/dodge beams
	if( selectedEnemies.AreAboutToHitLGorPG() ) {
		return true;
	}

	// As its fairly rarely gets really detected, always return true in this case
	// (we tried first to apply an additional distance cutoff)
	return selectedEnemies.AreAboutToHitRLorSW();
}

bool Bot::IsCombatDashingAllowed() const {
	// Should not be called with this enemies state but lets return a feasible value for this case
	if( !selectedEnemies.AreValid() ) {
		return true;
	}

	// AD-AD spam vs a quad is pointless, the bot should flee away
	if( selectedEnemies.HaveQuad() ) {
		return true;
	}

	if( const auto *hazard = PrimaryHazard() ) {
		// Always dash avoiding projectiles
		if( hazard->IsSplashLike() ) {
			return true;
		}
	}

	// Avoid RL/EB shots
	if( selectedEnemies.AreAboutToHitRLorSW() || selectedEnemies.AreAboutToHitEBorIG() ) {
		return true;
	}

	// Allow dashing for gaining speed to change a position
	return WillAdvance() || WillRetreat();
}

bool Bot::IsCombatCrouchingAllowed() const {
	if( !selectedEnemies.AreValid() ) {
		return false;
	}

	// If they're with EB and IG and are about to hit me
	if( selectedEnemies.AreAboutToHitEBorIG() ) {
		if( !selectedEnemies.AreAboutToHitRLorSW() && !selectedEnemies.AreAboutToHitLGorPG() ) {
			return true;
		}
	}

	return false;
}

bool Bot::TryGetExtraComputationQuota() const {
	return MillisInBlockedState() < 100 && AiManager::Instance()->TryGetExpensiveComputationQuota( this );
}

bool Bot::TryGetVitalComputationQuota() const {
	return AiManager::Instance()->TryGetExpensiveComputationQuota( this );
}

bool Bot::TryGetExpensiveThinkCallQuota() const {
	return AiManager::Instance()->TryGetExpensiveThinkCallQuota( this );
}