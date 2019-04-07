#ifndef QFUSION_BOT_GOALS_H
#define QFUSION_BOT_GOALS_H

#include "Planner.h"

class BotPlanningModule;

class BotGoal : public AiGoal {
	StaticVector<AiAction *, Ai::MAX_PLANNER_ACTIONS> extraApplicableActions;
public:
	BotGoal( BotPlanningModule *module_, const char *name_, int debugColor_, unsigned updatePeriod_ );

	void AddExtraApplicableAction( AiAction *action ) {
		extraApplicableActions.push_back( action );
	}
protected:
	BotPlanningModule *const module;

	PlannerNode *ApplyExtraActions( PlannerNode *firstTransition, const WorldState &worldState );

	Bot *Self() { return (Bot *)self; }
	const Bot *Self() const { return (Bot *)self; }

	const class SelectedNavEntity &SelectedNavEntity() const;
	const class SelectedEnemies &SelectedEnemies() const;
	const class BotWeightConfig &WeightConfig() const;
};

class GrabItemGoal : public BotGoal {
public:
	explicit GrabItemGoal( BotPlanningModule *module_ )
		: BotGoal( module_, "GrabItemGoal", COLOR_RGB( 0, 255, 0 ), 950 ) {}

	void UpdateWeight( const WorldState &currWorldState ) override;
	void GetDesiredWorldState( WorldState *worldState ) override;
	PlannerNode *GetWorldStateTransitions( const WorldState &worldState ) override;
};

class KillEnemyGoal : public BotGoal {
	float additionalWeight { 0.0f };
public:
	explicit KillEnemyGoal( BotPlanningModule *module_ )
		: BotGoal( module_, "KillEnemyGoal", COLOR_RGB( 255, 0, 0 ), 1250 ) {}

	void SetAdditionalWeight( float weight ) {
		this->additionalWeight = weight;
	}

	float GetAndResetAdditionalWeight() {
		float result = std::max( 0.0f, additionalWeight );
		this->additionalWeight = 0;
		return result;
	}

	void UpdateWeight( const WorldState &currWorldState ) override;
	void GetDesiredWorldState( WorldState *worldState ) override;
	PlannerNode *GetWorldStateTransitions( const WorldState &worldState ) override;
};

class RunAwayGoal : public BotGoal {
public:
	explicit RunAwayGoal( BotPlanningModule *module_ )
		: BotGoal( module_, "RunAwayGoal", COLOR_RGB( 0, 0, 255 ), 950 ) {}

	void UpdateWeight( const WorldState &currWorldState ) override;
	void GetDesiredWorldState( WorldState *worldState ) override;
	PlannerNode *GetWorldStateTransitions( const WorldState &worldState ) override;
};

class AttackOutOfDespairGoal : public BotGoal {
	float oldOffensiveness { 1.0f };
public:
	explicit AttackOutOfDespairGoal( BotPlanningModule *module_ )
		: BotGoal( module_, "AttackOutOfDespairGoal", COLOR_RGB( 192, 192, 0 ), 750 ) {}

	void UpdateWeight( const WorldState &currWorldState ) override;
	void GetDesiredWorldState( WorldState *worldState ) override;
	PlannerNode *GetWorldStateTransitions( const WorldState &worldState ) override;

	void OnPlanBuildingStarted() override;
	void OnPlanBuildingCompleted( const AiActionRecord *planHead ) override;
};

class ReactToHazardGoal : public BotGoal {
public:
	explicit ReactToHazardGoal( BotPlanningModule *module_ )
		: BotGoal( module_, "ReactToHazardGoal", COLOR_RGB( 192, 0, 192 ), 750 ) {}

	void UpdateWeight( const WorldState &currWorldState ) override;
	void GetDesiredWorldState( WorldState *worldState ) override;
	PlannerNode *GetWorldStateTransitions( const WorldState &worldState ) override;
};

class ReactToThreatGoal : public BotGoal {
public:
	explicit ReactToThreatGoal( BotPlanningModule *module_ )
		: BotGoal( module_, "ReactToThreatGoal", COLOR_RGB( 255, 0, 128 ), 350 ) {}

	void UpdateWeight( const WorldState &currWorldState ) override;
	void GetDesiredWorldState( WorldState *worldState ) override;
	PlannerNode *GetWorldStateTransitions( const WorldState &worldState ) override;
};

class ReactToEnemyLostGoal : public BotGoal {
public:
	explicit ReactToEnemyLostGoal( BotPlanningModule *module_ )
		: BotGoal( module_, "ReactToEnemyLostGoal", COLOR_RGB( 0, 192, 192 ), 950 ) {}

	void UpdateWeight( const WorldState &currWorldState ) override;
	void GetDesiredWorldState( WorldState *worldState ) override;
	PlannerNode *GetWorldStateTransitions( const WorldState &worldState ) override;
};

class RoamGoal : public BotGoal {
public:
	explicit RoamGoal( BotPlanningModule *module_ )
		: BotGoal( module_, "RoamGoal", COLOR_RGB( 0, 0, 80 ), 400 ) {}

	void UpdateWeight( const WorldState &currWorldState ) override;
	void GetDesiredWorldState( WorldState *worldState ) override;
	PlannerNode *GetWorldStateTransitions( const WorldState &worldState ) override;
};

class BotScriptGoal : public BotGoal {
	void *scriptObject;
public:
	// TODO: Provide ways for setting the debug color for this kind of goals
	explicit BotScriptGoal( BotPlanningModule *module_, const char *name_, unsigned updatePeriod_, void *scriptObject_ )
		: BotGoal( module_, name_, 0, updatePeriod_ ),
		scriptObject( scriptObject_ ) {}

	// Exposed for script API
	using BotGoal::Self;

	void UpdateWeight( const WorldState &currWorldState ) override;
	void GetDesiredWorldState( WorldState *worldState ) override;
	PlannerNode *GetWorldStateTransitions( const WorldState &worldState ) override;

	void OnPlanBuildingStarted() override;
	void OnPlanBuildingCompleted( const AiActionRecord *planHead ) override;
};

#endif
