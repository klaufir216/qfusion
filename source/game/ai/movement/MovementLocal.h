#ifndef QFUSION_MOVEMENTLOCAL_H
#define QFUSION_MOVEMENTLOCAL_H

#include "../bot.h"

#include "MovementPredictionContext.h"
#include "EnvironmentTraceCache.h"

// For macOS Clang
#include <cmath>
#include <cstdlib>

#ifndef PUBLIC_BUILD
#define CHECK_ACTION_SUGGESTION_LOOPS
#define ENABLE_MOVEMENT_ASSERTIONS
#define CHECK_INFINITE_NEXT_STEP_LOOPS
extern int nextStepIterationsCounter;
static constexpr int NEXT_STEP_INFINITE_LOOP_THRESHOLD = 10000;
#endif

// Useful for debugging but freezes even Release version
#if 0
#define ENABLE_MOVEMENT_DEBUG_OUTPUT
#endif

// Should be applied to a view vector Z to avoid bending (but does not suit all cases).
constexpr float Z_NO_BEND_SCALE = 0.5f;
// A threshold of dot product of velocity dir and intended view dir
constexpr float STRAIGHT_MOVEMENT_DOT_THRESHOLD = 0.8f;

inline float GetPMoveStatValue( const player_state_t *playerState, int statIndex, float defaultValue ) {
	float value = playerState->pmove.stats[statIndex];
	// Put likely case (the value is not specified) first
	return value < 0 ? defaultValue : value;
}

inline float MovementPredictionContext::GetJumpSpeed() const {
	return GetPMoveStatValue( this->currPlayerState, PM_STAT_JUMPSPEED, DEFAULT_JUMPSPEED * GRAVITY_COMPENSATE );
}

inline float MovementPredictionContext::GetDashSpeed() const {
	return GetPMoveStatValue( this->currPlayerState, PM_STAT_DASHSPEED, DEFAULT_DASHSPEED );
}

inline float MovementPredictionContext::GetRunSpeed() const {
	return GetPMoveStatValue( this->currPlayerState, PM_STAT_MAXSPEED, DEFAULT_PLAYERSPEED );
}

inline unsigned MovementPredictionContext::DefaultFrameTime() const {
	return defaultFrameTime;
}

inline Vec3 MovementPredictionContext::NavTargetOrigin() const {
	return bot->NavTargetOrigin();
}

inline float MovementPredictionContext::NavTargetRadius() const {
	return bot->NavTargetRadius();
}

inline bool MovementPredictionContext::IsCloseToNavTarget() const {
	float distance = NavTargetRadius() + 32.0f;
	return NavTargetOrigin().SquareDistanceTo( movementState->entityPhysicsState.Origin() ) < distance * distance;
}

inline int MovementPredictionContext::CurrAasAreaNum() const {
	if( int currAasAreaNum = movementState->entityPhysicsState.CurrAasAreaNum() ) {
		return currAasAreaNum;
	}

	return movementState->entityPhysicsState.DroppedToFloorAasAreaNum();
}

inline int MovementPredictionContext::CurrGroundedAasAreaNum() const {
	const auto *aasWorld = AiAasWorld::Instance();
	const auto &entityPhysicsState = movementState->entityPhysicsState;
	int areaNums[2] = { entityPhysicsState.CurrAasAreaNum(), entityPhysicsState.DroppedToFloorAasAreaNum() };
	for( int i = 0, end = ( areaNums[0] != areaNums[1] ? 2 : 1 ); i < end; ++i ) {
		if( areaNums[i] && aasWorld->AreaGrounded( areaNums[i] ) ) {
			return areaNums[i];
		}
	}
	return 0;
}

inline int MovementPredictionContext::NavTargetAasAreaNum() const {
	return bot->NavTargetAasAreaNum();
}

inline bool MovementPredictionContext::IsInNavTargetArea() const {
	const int navTargetAreaNum = NavTargetAasAreaNum();
	if( !navTargetAreaNum ) {
		return false;
	}

	const auto &entityPhysicsState = this->movementState->entityPhysicsState;
	if( navTargetAreaNum == entityPhysicsState.CurrAasAreaNum() ) {
		return true;
	}

	if( navTargetAreaNum == entityPhysicsState.DroppedToFloorAasAreaNum() ) {
		return true;
	}

	return false;
}

inline bool IsInsideHugeArea( const float *origin, const aas_area_t &area, float offset ) {
	if( area.mins[0] > origin[0] - offset || area.maxs[0] < origin[0] + offset ) {
		return false;
	}

	if( area.mins[1] > origin[1] - offset || area.maxs[1] < origin[1] + offset ) {
		return false;
	}

	return true;
}

inline void MovementPredictionContext::Assert( bool condition, const char *message ) const {
#ifdef ENABLE_MOVEMENT_ASSERTIONS
	if( !condition ) {
		if( message ) {
			AI_FailWith( "MovementPredictionContext::Assert()", "%s\n", message );
		} else {
			AI_FailWith( "MovementPredictionContext::Assert()", "An assertion has failed\n" );
		}
	}
#endif
}

inline void BaseMovementAction::Assert( bool condition, const char *message ) const {
#ifdef ENABLE_MOVEMENT_ASSERTIONS
	if( !condition ) {
		if( message ) {
			AI_FailWith("BaseMovementAction::Assert()", "An assertion has failed: %s\n", message );
		} else {
			AI_FailWith("BaseMovementAction::Assert()", "An assertion has failed\n");
		}
	}
#endif
}

inline const AiAasRouteCache *MovementPredictionContext::RouteCache() const {
	return bot->RouteCache();
}

inline const ArrayRange<int> MovementPredictionContext::TravelFlags() const {
	return bot->TravelFlags();
}

inline EnvironmentTraceCache &MovementPredictionContext::TraceCache() {
	return environmentTestResultsStack.back();
}

inline void MovementPredictionContext::SaveActionOnStack( BaseMovementAction *action ) {
	auto *topOfStack = &this->predictedMovementActions[this->topOfStackIndex];
	// This was a source of an annoying bug! movement state has been modified during a prediction step!
	// We expect that record state is a saved state BEFORE the step!
	//topOfStack->entityPhysicsState = this->movementState->entityPhysicsState;
	topOfStack->action = action;
	// Make sure the angles can always be modified for input interpolation or aiming
	topOfStack->record.botInput.hasAlreadyComputedAngles = false;
	topOfStack->timestamp = this->totalMillisAhead;

#ifdef ENABLE_MOVEMENT_ASSERTIONS
	constexpr auto *tag = "MovementPredictionContext::SaveActionOnStack()";
	if( !action ) {
		AI_FailWith( tag, "The action is null\n" );
	}
	if( this->predictionStepMillis > 100 ) {
		const char *format =
			"%s: The prediction step millis value %u is way too large. "
			"Is it a result of wrapping of negative values in unsigned context?\n";
		AI_FailWith( tag, format, action->Name() );
	}
	if( this->predictionStepMillis % DefaultFrameTime() ) {
		const char *format = "%s: The prediction step millis value %u is no a multiple of %u\n";
		AI_FailWith( tag, format, action->Name(), this->predictionStepMillis, DefaultFrameTime() );
	}
#endif

	topOfStack->stepMillis = this->predictionStepMillis;
	this->topOfStackIndex++;
}

inline const char *MovementPredictionContext::ActiveActionName() const {
	return activeAction ? activeAction->Name() : nullptr;
}

inline void MovementPredictionContext::MarkSavepoint( BaseMovementAction *markedBy, unsigned frameIndex ) {
#ifdef ENABLE_MOVEMENT_ASSERTIONS
	constexpr auto *tag = "MovementPredictionContext::MarkSavepoint()";
	if( !markedBy ) {
		AI_FailWith( tag, "`markedBy` action is null\n" );
	}
	if( this->cannotApplyAction ) {
		constexpr auto *format = "%s: Attempt to mark a savepoint while `cannotApplyAction` context flag is set\n";
		AI_FailWith( tag, format, markedBy->Name() );
	}
	if( this->shouldRollback ) {
		constexpr auto *format = "%s: Attempt to mark a savepoint while `shouldRollback` context flag is set\n";
		AI_FailWith( tag, format, markedBy->Name() );
	}
	if( frameIndex != this->topOfStackIndex && frameIndex != this->topOfStackIndex + 1 ) {
		constexpr auto *format =
			"%s: Attempt to mark a savepoint at index %d while ToS index is %d:"
			" the savepoint index must be the same or be a first next index\n";
		AI_FailWith( tag, format, markedBy->Name(), frameIndex, this->topOfStackIndex );
	}
#endif

	this->savepointTopOfStackIndex = frameIndex;
	Debug( "%s has marked frame %d as a savepoint\n", markedBy->Name(), frameIndex );
}

inline void MovementPredictionContext::SetPendingRollback() {
	this->cannotApplyAction = true;
	this->shouldRollback = true;
}

inline void MovementPredictionContext::RollbackToSavepoint() {
#ifdef ENABLE_MOVEMENT_ASSERTIONS
	constexpr auto *tag = "MovementPredictionContext::RollbackToSavepoint()";
	if( this->isCompleted ) {
		constexpr auto *format = "%s: Attempt to rollback while the context is in completed state\n";
		AI_FailWith( tag, format, ActiveActionName() );
	}
	if( !this->shouldRollback ) {
		constexpr auto *format = "%s: Attempt to rollback while `shouldRollback` context flag is not set\n";
		AI_FailWith( tag, format, ActiveActionName() );
	}
	if( !this->cannotApplyAction ) {
		constexpr auto *format = "%s: Attempt to rollback while `cannotApplyAction` context flag is not set\n";
		AI_FailWith( tag, format, ActiveActionName() );
	}
	if( this->savepointTopOfStackIndex > this->topOfStackIndex ) {
		constexpr auto *format = "The savepoint index %u is greater than the current ToS index %u\n";
		AI_FailWith( tag, format, this->savepointTopOfStackIndex, this->topOfStackIndex );
	}
#endif

	constexpr const char *format = "Rolling back to savepoint frame %d from ToS frame %d\n";
	Debug( format, this->savepointTopOfStackIndex, this->topOfStackIndex );
	this->topOfStackIndex = this->savepointTopOfStackIndex;
}

inline void MovementPredictionContext::SaveSuggestedActionForNextFrame( BaseMovementAction *action ) {
	//Assert(!this->actionSuggestedByAction);
	this->actionSuggestedByAction = action;
}

inline unsigned MovementPredictionContext::MillisAheadForFrameStart( unsigned frameIndex ) const {
#ifdef ENABLE_MOVEMENT_ASSERTIONS
	constexpr auto *tag = "MovementPredictionContext::MillisAheadForFrameStart()";
	constexpr auto *format = "The frame index %u must not be greater than the current ToS index %u\n";
	if( frameIndex > topOfStackIndex ) {
		AI_FailWith( tag, format, frameIndex, topOfStackIndex );
	}
#endif
	if( frameIndex < topOfStackIndex ) {
		return (unsigned)( predictedMovementActions[frameIndex].timestamp );
	}
	return totalMillisAhead;
}

inline ObstacleAvoidanceResult MovementPredictionContext::TryAvoidFullHeightObstacles( float correctionFraction ) {
	// Make a modifiable copy of the intended look dir
	Vec3 intendedLookVec( this->record->botInput.IntendedLookDir() );
	auto result = EnvironmentTraceCache().TryAvoidFullHeightObstacles( this, &intendedLookVec, correctionFraction );
	if( result == ObstacleAvoidanceResult::CORRECTED ) {
		// Write the modified intended look dir back in this case
		this->record->botInput.SetIntendedLookDir( intendedLookVec );
	}
	return result;
}

inline ObstacleAvoidanceResult MovementPredictionContext::TryAvoidJumpableObstacles( float correctionFraction ) {
	// Make a modifiable copy of the intended look dir
	Vec3 intendedLookVec( this->record->botInput.IntendedLookDir() );
	auto result = EnvironmentTraceCache().TryAvoidJumpableObstacles( this, &intendedLookVec, correctionFraction );
	if( result == ObstacleAvoidanceResult::CORRECTED ) {
		// Write the modified intended look dir back in this case
		this->record->botInput.SetIntendedLookDir( intendedLookVec );
	}
	return result;
}

inline BaseMovementAction &BaseMovementAction::DummyAction() {
	// We have to check the combat action since it might be disabled due to planning stack overflow.
	if( bot->ShouldKeepXhairOnEnemy() && bot->GetSelectedEnemies().AreValid() ) {
		if( !module->combatDodgeSemiRandomlyToTargetAction.IsDisabledForPlanning() ) {
			return module->combatDodgeSemiRandomlyToTargetAction;
		}
	}

	return module->fallbackMovementAction;
}

inline BaseMovementAction &BaseMovementAction::DefaultWalkAction() {
	return module->walkCarefullyAction;
}

inline BaseMovementAction &BaseMovementAction::DefaultBunnyAction() {
	return module->bunnyToStairsOrRampExitAction;
}

inline BaseMovementAction &BaseMovementAction::FallbackBunnyAction() {
	return module->walkOrSlideInterpolatingReachChainAction;
}

inline FlyUntilLandingAction &BaseMovementAction::FlyUntilLandingAction() {
	return module->flyUntilLandingAction;
}

inline LandOnSavedAreasAction &BaseMovementAction::LandOnSavedAreasAction() {
	return module->landOnSavedAreasAction;
}

inline bool BaseMovementAction::GenericCheckIsActionEnabled( MovementPredictionContext *context,
															 BaseMovementAction *suggestedAction ) const {
	// Put likely case first
	if( !isDisabledForPlanning ) {
		return true;
	}

	context->sequenceStopReason = DISABLED;
	context->cannotApplyAction = true;
	context->actionSuggestedByAction = suggestedAction;
	Debug( "The action has been completely disabled for further planning\n" );
	return false;
}

typedef MovementPredictionContext Context;

inline void BaseMovementAction::DisableWithAlternative( Context *context, BaseMovementAction *suggestedAction ) {
	context->cannotApplyAction = true;
	context->actionSuggestedByAction = suggestedAction;
	this->isDisabledForPlanning = true;
}

inline void BaseMovementAction::SwitchOrStop( Context *context, BaseMovementAction *suggestedAction ) {
	// Few predicted frames are enough if the action cannot be longer applied (but have not caused rollback)
	if( context->topOfStackIndex > 0 ) {
		Debug( "There were enough successfully predicted frames anyway, stopping prediction\n" );
		context->isCompleted = true;
		return;
	}

	DisableWithAlternative( context, suggestedAction );
}

inline void BaseMovementAction::SwitchOrRollback( Context *context, BaseMovementAction *suggestedAction ) {
	if( context->topOfStackIndex > 0 ) {
		Debug( "There were some frames predicted ahead that lead to a failure, should rollback\n" );
		this->isDisabledForPlanning = true;
		context->SetPendingRollback();
		return;
	}

	DisableWithAlternative( context, suggestedAction );
}

inline float Distance2DSquared( const vec3_t a, const vec3_t b ) {
	float dx = a[0] - b[0];
	float dy = a[1] - b[1];
	return dx * dx + dy * dy;
}

#ifndef SQUARE
#define SQUARE( x ) ( ( x ) * ( x ) )
#endif

static inline bool ShouldCrouchSlideNow( MovementPredictionContext *context ) {
	if( !( context->currPlayerState->pmove.stats[PM_STAT_FEATURES] & PMFEAT_CROUCHSLIDING ) ) {
		return false;
	}

	if( context->currPlayerState->pmove.pm_flags & PMF_CROUCH_SLIDING ) {
		if( context->currPlayerState->pmove.stats[PM_STAT_CROUCHSLIDETIME] > PM_CROUCHSLIDE_FADE ) {
			return true;
		}
	}

	if( context->movementState->entityPhysicsState.Speed2D() > context->GetRunSpeed() * 1.2f ) {
		return true;
	}

	return false;
}

// Height threshold should be set according to used time step
// (we might miss crouch sliding activation if its low and the time step is large)
inline bool ShouldPrepareForCrouchSliding( MovementPredictionContext *context, float heightThreshold = 12.0f ) {
	if( !(context->currPlayerState->pmove.stats[PM_STAT_FEATURES ] & PMFEAT_CROUCHSLIDING ) ) {
		return false;
	}

	const auto &entityPhysicsState = context->movementState->entityPhysicsState;
	if( entityPhysicsState.GroundEntity() ) {
		return false;
	}

	if( entityPhysicsState.Velocity()[2] > 0 ) {
		return false;
	}

	if( entityPhysicsState.HeightOverGround() > heightThreshold ) {
		return false;
	}

	if( entityPhysicsState.Speed() < context->GetRunSpeed() ) {
		return false;
	}

	return true;
}

class TriggerAreaNumsCache {
	mutable int areaNums[MAX_EDICTS];
public:
	TriggerAreaNumsCache() {
		memset( areaNums, 0, sizeof( areaNums ) );
	}

	int GetAreaNum( int entNum ) const;
};

extern TriggerAreaNumsCache triggerAreaNumsCache;

int TravelTimeWalkingOrFallingShort( const AiAasRouteCache *routeCache, int fromAreaNum, int toAreaNum );

/**
 * Serves for candidate spots selection.
 * Tracing a straight line between two points fails in stairs-like environment way too often.
 * This routine uses extremely coarse arc approximation which still should be sufficient
 * to avoid the mentioned failure in some environment kinds.
 */
bool TraceArcInSolidWorld( const AiEntityPhysicsState &startPhysicsState, const vec3_t from, const vec3_t to );

#endif
