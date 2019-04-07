#ifndef QFUSION_AI_BASE_BRAIN_H
#define QFUSION_AI_BASE_BRAIN_H

#include "../ai_local.h"
#include "GoalEntities.h"
#include "Pool.h"
#include "../ai_frame_aware_updatable.h"
#include "../static_vector.h"
#include "../navigation/AasRouteCache.h"
#include "../ai_base_ai.h"
#include "WorldState.h"

class AiGoal {
	friend class Ai;
	friend class AiPlanner;
protected:
	Ai *const self;
	const char *name;
	const unsigned updatePeriod;
	int debugColor { 0 };
	float weight { 0.0f };

public:
	AiGoal( Ai *self_, const char *name_, unsigned updatePeriod_ )
		: self( self_ ), name( name_ ), updatePeriod( updatePeriod_ ) {}

	virtual ~AiGoal() = default;

	virtual void UpdateWeight( const WorldState &worldState ) = 0;
	virtual void GetDesiredWorldState( WorldState *worldState ) = 0;
	virtual struct PlannerNode *GetWorldStateTransitions( const WorldState &worldState ) = 0;

	virtual void OnPlanBuildingStarted() {}
	virtual void OnPlanBuildingCompleted( const class AiActionRecord *planHead ) {}

	bool IsRelevant() const { return weight > 0; }

	// More important goals are first after sorting goals array
	bool operator<( const AiGoal &that ) const {
		return this->weight > that.weight;
	}

	int DebugColor() const { return debugColor; }

	const char *Name() const { return name; }
	unsigned UpdatePeriod() const { return updatePeriod; }
};

class AiActionRecord : public PoolItem {
	friend class AiBaseAction;
protected:
	Ai *const self;
	const char *name;

#ifndef _MSC_VER
	inline void Debug( const char *format, ... ) const __attribute__( ( format( printf, 2, 3 ) ) )
#else
	inline void Debug( _Printf_format_string_ const char *format, ... ) const
#endif
	{
		va_list va;
		va_start( va, format );
		AI_Debugv( name, format, va );
		va_end( va );
	}

public:
	AiActionRecord *nextInPlan { nullptr };

	AiActionRecord( PoolBase *pool_, Ai *self_, const char *name_ )
		: PoolItem( pool_ ), self( self_ ), name( name_ ) {}

	virtual void Activate() {
		Debug( "About to activate\n" );
	};

	virtual void Deactivate() {
		Debug( "About to deactivate\n" );
	};

	const char *Name() const { return name; }

	enum Status {
		INVALID,
		VALID,
		COMPLETED
	};

	virtual Status UpdateStatus( const WorldState &currWorldState ) = 0;
};

struct PlannerNode : PoolItem {
	// World state after applying an action
	WorldState worldState;
	// An action record to apply
	AiActionRecord *actionRecord { nullptr };
	// Used to reconstruct a plan
	PlannerNode *parent { nullptr };
	// Next in linked list of transitions for current node
	PlannerNode *nextTransition { nullptr };

	// Utilities for storing the node in a hash set
	PlannerNode *prevInHashBin { nullptr };
	PlannerNode *nextInHashBin { nullptr };

	// An A-star edge "distance"
	float transitionCost { std::numeric_limits<float>::max() };
	// An A-star node "G"
	float costSoFar { std::numeric_limits<float>::max() };
	// A priority queue parameter
	float heapCost { std::numeric_limits<float>::max() };
	// An utility for retrieval an actual index in heap array by a node value
	unsigned heapArrayIndex { std::numeric_limits<unsigned>::max() };

	// A hash of the associated world state (put here for optimal members alignment)
	uint32_t worldStateHash { 0 };

	inline PlannerNode( PoolBase *pool, Ai *self );

	~PlannerNode() override {
		if( actionRecord ) {
			actionRecord->DeleteSelf();
		}

#ifndef PUBLIC_BUILD
		// Prevent use-after-free.
		actionRecord = nullptr;
		parent = nullptr;
		nextTransition = nullptr;
		prevInHashBin = nullptr;
		nextInHashBin = nullptr;
#endif
	}
};

class AiAction {
	friend class Ai;
protected:
	Ai *self;
	const char *name;

#ifndef _MSC_VER
	inline void Debug( const char *format, ... ) const __attribute__( ( format( printf, 2, 3 ) ) )
#else
	inline void Debug( _Printf_format_string_ const char *format, ... ) const
#endif
	{
		va_list va;
		va_start( va, format );
		AI_Debugv( name, format, va );
		va_end( va );
	}

	class PlannerNodePtr {
		PlannerNode *node;
	public:
		PlannerNodePtr( const PlannerNodePtr &that ) = delete;
		PlannerNodePtr &operator=( const PlannerNodePtr &that ) = delete;

		explicit PlannerNodePtr( PlannerNode *node_ ) : node( node_ ) {}

		PlannerNodePtr( PlannerNodePtr &&that ) : node( that.node ) {
			that.node = nullptr;
		}

		PlannerNodePtr &operator=( PlannerNodePtr &&that ) {
			node = that.node;
			that.node = nullptr;
			return *this;
		}

		PlannerNode *ReleaseOwnership() {
			PlannerNode *result = node;
			// Clear node reference to avoid being deleted in the destructor
			node = nullptr;
			return result;
		}

		inline ~PlannerNodePtr();
		inline PlannerNode *PrepareActionResult();
		inline class WorldState &WorldState();
		inline float &Cost();
		operator bool() const { return node != nullptr; }
	};

	PlannerNodePtr NewNodeForRecord( AiActionRecord *record );
public:
	AiAction( Ai *self_, const char *name_ )
		: self( self_ ), name( name_ ) {}

	virtual ~AiAction() = default;

	const char *Name() const { return name; }

	virtual PlannerNode *TryApply( const WorldState &worldState ) = 0;
};

class AiPlanner : public AiFrameAwareUpdatable {
	friend class Ai;
	friend class AiManager;
	friend class AiBaseTeam;
	friend class AiGoal;
	friend class AiAction;
	friend class AiActionRecord;

public:
	static constexpr unsigned MAX_GOALS = 12;
	static constexpr unsigned MAX_ACTIONS = 36;

protected:
	Ai *const ai;

	AiActionRecord *planHead { nullptr };
	AiGoal *activeGoal { nullptr };
	int64_t nextActiveGoalUpdateAt { 0 };

	StaticVector<AiGoal *, MAX_GOALS> goals;
	StaticVector<AiAction *, MAX_ACTIONS> actions;

	static constexpr unsigned MAX_PLANNER_NODES = 384;
	Pool<PlannerNode, MAX_PLANNER_NODES> plannerNodesPool { "PlannerNodesPool" };

	explicit AiPlanner( Ai *ai_ ): ai( ai_ ) {}

	virtual void PrepareCurrWorldState( WorldState *worldState ) = 0;

	virtual bool ShouldSkipPlanning() const = 0;

	bool UpdateGoalAndPlan( const WorldState &currWorldState );

	bool FindNewGoalAndPlan( const WorldState &currWorldState );

	// Allowed to be overridden in a subclass for class-specific optimization purposes
	virtual AiActionRecord *BuildPlan( AiGoal *goal, const WorldState &startWorldState );

	AiActionRecord *ReconstructPlan( PlannerNode *lastNode ) const;

	void SetGoalAndPlan( AiGoal *goal_, AiActionRecord *planHead_ );

	void Think() override;

	virtual void BeforePlanning() {}
	virtual void AfterPlanning() {}
public:
	bool HasPlan() const { return planHead != nullptr; }

	void ClearGoalAndPlan();

	void DeletePlan( AiActionRecord *head );
};

inline AiAction::PlannerNodePtr::~PlannerNodePtr() {
	if( this->node ) {
		this->node->DeleteSelf();
	}
}

inline PlannerNode *AiAction::PlannerNodePtr::PrepareActionResult() {
	PlannerNode *result = this->node;
	this->node = nullptr;

#ifndef PUBLIC_BUILD
	if( !result->worldState.IsCopiedFromOtherWorldState() ) {
		AI_FailWith( "PlannerNodePtr::PrepareActionResult()", "World state has not been copied from parent one" );
	}
#endif

	// Compute modified world state hash
	// This computation have been put here to avoid error-prone copy-pasting.
	// Another approach is to use lazy hash code computation but it adds branching on each hash code access
	result->worldStateHash = result->worldState.Hash();
	return result;
}

inline WorldState &AiAction::PlannerNodePtr::WorldState() {
	return node->worldState;
}

inline float &AiAction::PlannerNodePtr::Cost() {
	return node->transitionCost;
}

#endif
