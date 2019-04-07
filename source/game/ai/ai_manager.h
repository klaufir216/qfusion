#ifndef QFUSION_AI_MANAGER_H
#define QFUSION_AI_MANAGER_H

#include "planning/Planner.h"
#include "ai_frame_aware_updatable.h"
#include "planning/GoalEntities.h"
#include "static_vector.h"

class Bot;

class AiManager : public AiFrameAwareUpdatable
{
protected:
	AiManager( const char *gametype, const char *mapname );

	int teams[MAX_CLIENTS];
	ai_handle_t *aiHandlesListHead { nullptr };

	struct Quota {
		int64_t givenAt { 0 };
		const ai_handle_t *owner { nullptr };

		virtual bool Fits( const ai_handle_t *ai ) const = 0;

		bool TryAcquire( const ai_handle_t *ai );
		void Update( const ai_handle_t *aiHandlesHead );

		void OnRemoved( const ai_handle_t *ai ) {
			if( ai == owner ) {
				owner = nullptr;
			}
		}
	};

	struct GlobalQuota final : public Quota {
		bool Fits( const ai_handle_t *ai ) const override;
	};

	struct ThinkQuota final : public Quota {
		const unsigned affinityOffset;
		explicit ThinkQuota( unsigned affinityOffset_ ): affinityOffset( affinityOffset_ ) {}
		bool Fits( const ai_handle_t *ai ) const override;
	};

	GlobalQuota globalCpuQuota;
	ThinkQuota thinkQuota[4] = {
		ThinkQuota( 0 ), ThinkQuota( 1 ), ThinkQuota( 2 ), ThinkQuota( 3 )
	};

	int hubAreas[16];
	int numHubAreas { 0 };

	static AiManager *instance;

	void Frame() override;

	bool CheckCanSpawnBots();
	void CreateUserInfo( char *buffer, size_t bufferSize );
	edict_t * ConnectFakeClient();
	float MakeSkillForNewBot( const gclient_t *client ) const;
	void SetupBotForEntity( edict_t *ent );
	void TryJoiningTeam( edict_t *ent, const char *teamName );

	/**
	 * There are no reasons to use a trie.
	 * This code might be slow but is called only when a bot enters a game.
	 * Tries are not convenient in use for this case and waste heap memory a lot.
	 */
	template <typename T, unsigned N>
	class StringValueMap {
		using value_type = std::pair<const char *, T>;

		StaticVector<value_type, N> keyValuePairs;
		unsigned clearLimit { 0 };
public:
		using iterator = value_type *;
		using const_iterator = const value_type *;

		~StringValueMap() {
			ClearToLimit();
		}

		T *Get( const char *key );

		/**
		 * The key must be valid during the entire StringValueMap object lifetime.
		 * The key lifetime must be managed by an external (caller) code.
		 */
		bool Insert( const char *key, T &&value );

		bool IsFull() const { return keyValuePairs.size() == N; }

		void MarkClearLimit() { clearLimit = keyValuePairs.size(); }

		void ClearToLimit() {
			while( keyValuePairs.size() != clearLimit ) {
				keyValuePairs.pop_back();
			}
		}

		iterator begin() { return keyValuePairs.begin(); }
		iterator end() { return keyValuePairs.end(); };
		const_iterator begin() const { return keyValuePairs.begin(); }
		const_iterator end() const { return keyValuePairs.end(); };
	};

	struct ActionProps {
		char *name { nullptr };
		// Null if builtin
		void *factoryObject { nullptr };

		ActionProps() = default;

		ActionProps( const char *name_, void *factoryObject_ ) {
			auto nameLen = strlen( name_ );
			name = (char *)G_Malloc( nameLen + 1 );
			memcpy( name, name_, nameLen + 1 );
			factoryObject = factoryObject_;
		}

		~ActionProps() {
			if( name ) {
				G_Free( name );
			}
		}

		ActionProps( const ActionProps &that ) = delete;
		ActionProps &operator=( const ActionProps &that ) = delete;

		ActionProps( ActionProps &&that ) {
			*this = std::move( that );
		}

		ActionProps& operator=( ActionProps &&that ) {
			name = that.name;
			that.name = nullptr;
			factoryObject = that.factoryObject;
			return *this;
		}
	};

	struct GoalProps {
		char *name { nullptr };
		// Null if builtin
		void *factoryObject { nullptr };
		unsigned updatePeriod { 0 };
		// Can't use StaticVector due to its disabled copy/move ctors
		const char *applicableActions[Ai::MAX_PLANNER_ACTIONS];
		unsigned numApplicableActions { 0 };

		GoalProps() = default;

		GoalProps( const char *name_, void *factoryObject_, unsigned updatePeriod_ ) {
			auto nameLen = strlen( name_ );
			name = (char *)G_Malloc( nameLen + 1 );
			memcpy( name, name_, nameLen + 1 );
			factoryObject = factoryObject_;
			updatePeriod = updatePeriod_;
		}

		GoalProps( const GoalProps &that ) = delete;
		GoalProps &operator=( const GoalProps &that ) = delete;

		GoalProps( GoalProps &&that ) {
			*this = std::move( that );
		}

		GoalProps& operator=( GoalProps &&that ) {
			name = that.name;
			that.name = nullptr;
			factoryObject = that.factoryObject;
			updatePeriod = that.updatePeriod;
#ifdef _DEBUG
			if( that.numApplicableActions != 0 ) {
				AI_FailWith( "GoalProps(GoalProps &&that)", "Wrong usage pattern. Do not copy non-empty GoalProps\n" );
			}
#endif
			std::copy_n( that.applicableActions, that.numApplicableActions, this->applicableActions );
			numApplicableActions = that.numApplicableActions;
			return *this;
		}

		~GoalProps() {
			if( name ) {
				G_Free( name );
			}
		}
	};

	StringValueMap<ActionProps, Ai::MAX_PLANNER_ACTIONS> registeredActions;
	StringValueMap<GoalProps, Ai::MAX_PLANNER_GOALS> registeredGoals;

	void RegisterBuiltinGoal( const char *goalName );
	void RegisterBuiltinAction( const char *actionName );

	void SetupBotGoalsAndActions( edict_t *ent );

	void FindHubAreas();
public:
	void LinkAi( ai_handle_t *aiHandle );
	void UnlinkAi( ai_handle_t *aiHandle );

	void OnBotDropped( edict_t *ent );

	static AiManager *Instance() { return instance; }

	static void Init( const char *gametype, const char *mapname );
	static void Shutdown();

	void NavEntityReachedBy( const NavEntity *canceledGoal, const class Ai *goalGrabber );
	void NavEntityReachedSignal( const edict_t *ent );
	void OnBotJoinedTeam( edict_t *ent, int team );

	void RegisterEvent( const edict_t *ent, int event, int parm );

	void SpawnBot( const char *teamName );
	void RespawnBot( edict_t *ent );
	void RemoveBot( const char *name );
	void AfterLevelScriptShutdown();
	void BeforeLevelScriptShutdown();

	void RegisterScriptGoal( const char *goalName, void *factoryObject, unsigned updatePeriod );
	void RegisterScriptAction( const char *actionName, void *factoryObject );
	void AddApplicableAction( const char *goalName, const char *actionName );

	inline void UnregisterScriptGoalsAndActions() {
		// There should not be any bots connected when this method gets called
		registeredGoals.ClearToLimit();
		registeredActions.ClearToLimit();
	}

	bool IsAreaReachableFromHubAreas( int targetArea, float *score = nullptr ) const;

	/**
	 * Allows cycling rights to perform CPU-consuming operations among bots.
	 * This is similar to checking ent == level.think_client_entity
	 * but counts only bots making cycling and thus frametimes more even.
	 * These calls have semantics similar to "compare and swap":
	 * If somebody has already requested an operation, returns false.
	 * Otherwise, sets some internal lock and returns true.
	 * @note Subsequent calls in the same frame fail even for the same client
	 * (only a single expensive operation is allowed per frame globally).
	 */
	bool TryGetExpensiveComputationQuota( const Bot *bot );

	/**
	 * Similar to {@code TryGetExpensiveComputationQuota()}
	 * but tracks bots that have different think frame offset separately.
	 * @note This quota is independent from the global one.
	 */
	bool TryGetExpensiveThinkCallQuota( const Bot *bot );
};

#endif
