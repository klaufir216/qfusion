#ifndef QFUSION_AI_BASE_TEAM_BRAIN_H
#define QFUSION_AI_BASE_TEAM_BRAIN_H

#include "../ai_frame_aware_updatable.h"

#include <typeinfo>

class NavEntity;

class AiBaseTeam : public AiFrameAwareUpdatable {
	friend class Bot;  // Bots should be able to notify its team in destructor when they get dropped immediately
	friend class AiManager;

	// We can't initialize these vars in constructor, because game exports may be not yet intialized.
	// These values are set to -1 in constructor and computed on demand
	mutable int svFps { -1 };
	mutable int svSkill { -1 };

	// These vars are used instead of AiFrameAwareUpdatable for lazy intiailization
	mutable int teamAffinityModulo { -1 };
	mutable int teamAffinityOffset { -1 };
	static constexpr int MAX_AFFINITY_OFFSET = 4;
	// This array contains count of bots that use corresponding offset for each possible affinity offset
	unsigned affinityOffsetsInUse[MAX_AFFINITY_OFFSET];

	// These arrays store copies of bot affinities to be able to access them even if the bot reference has been lost
	unsigned char botAffinityModulo[MAX_CLIENTS];
	unsigned char botAffinityOffsets[MAX_CLIENTS];

	unsigned AffinityModulo() const;
	unsigned TeamAffinityOffset() const;

	void InitTeamAffinity() const;  // Callers are const ones, and only mutable vars are modified

	static void CreateTeam( int teamNum );
	static void ReleaseTeam( int teamNum );

	// A factory method for team creation.
	// Instantiates appropriate kind of team for a current gametype.
	static AiBaseTeam *InstantiateTeam( int teamNum );

	static AiBaseTeam *teamsForNums[GS_MAX_TEAMS - 1];
protected:
	explicit AiBaseTeam( int teamNum_ );

	Bot *teamBotsHead { nullptr };

	const int teamNum;

	void AddBot( class Bot *bot );
	void RemoveBot( class Bot *bot );
	virtual void OnBotAdded( class Bot *bot ) {};
	virtual void OnBotRemoved( class Bot *bot ) {};

	/**
	 * Transfers a state from an existing team to this instance.
	 * Moving and not copying semantics is implied
	 * (actual data should be moved from {@code that} if possible (without a copy creation).
	 * @note overridden methods must call parent ones first.
	 */
	virtual void TransferStateFrom( AiBaseTeam *that );

	void AcquireBotFrameAffinity( int entNum );
	void ReleaseBotFrameAffinity( int entNum );
	void SetBotFrameAffinity( int bot, unsigned modulo, unsigned offset );

	inline int GetCachedCVar( int *cached, const char *name ) const {
		if( *cached == -1 ) {
			*cached = (int)trap_Cvar_Value( name );
		}
		return *cached;
	}

	inline int ServerFps() const { return GetCachedCVar( &svFps, "sv_fps" ); }
	inline int ServerSkill() const { return GetCachedCVar( &svSkill, "sv_skilllevel" ); }

	void Debug( const char *format, ... );

	static void CheckTeamNum( int teamNum );
	static AiBaseTeam **TeamRefForNum( int teamNum );
	static void Init();
	static void Shutdown();

	/**
	 * Selects a preferred pair given two pairs of weights (each is nullable).
	 * @param weights1 a first pair of nav weight and planning goal weight.
	 * @param weights2 a second pair of nav weight and planning goal weight.
	 * @return a preferred pair (null if both pairs are null).
	 */
	inline static const std::pair<float, float> *ChooseWeights( const std::pair<float, float> *weights1,
		                                                        const std::pair<float, float> *weights2 ) {
		if( !weights1 ) {
			return weights2;
		}
		if( !weights2 ) {
			return weights1;
		}
		// What to do is not obvious... lets favour planner goal weight over nav weight
		return weights1->second > weights2->second ? weights1 : weights2;
	}
public:
	static AiBaseTeam *GetTeamForNum( int teamNum );

	/**
	 * Allows to specify the expected team type (that defines the team feature set)
	 * and thus switch an AI team dynamically if advanced AI features are requested.
	 * The purpose of this method is to simplify gametype scripting.
	 * (if some script syscalls that assume a feature-reach AI team are executed).
	 */
	static AiBaseTeam *GetTeamForNum( int teamNum, const std::type_info &desiredType );

	/**
	 * Allows to override entity weights for bot items selection.
	 * @param bot a bot that must belong to this team.
	 * @param ent an entity that could have an overridden weight.
	 * @return a pair of nav item weight and pickup item planning goal weight, null if not overridden.
	 * @note for optimization purposes this does not get called for items.
	 * @note assume that the returned value is invalidated on next call.
	 */
	virtual const std::pair<float, float> *GetEntityWeights( const Bot *bot, const NavEntity *navEntity ) const {
		return nullptr;
	}

	/**
	 * This is a hint that helps to avoid bunch of {@code GetEntityWeights()} calls in a loop
	 * @param bot a bot that must belong to this team
	 * @return true if {@code GetEntityWeights()} should be called.
	 * @note a supertype method should be called first if overridden.
	 */
	virtual bool OverridesEntityWeights( const Bot *bot ) const { return false; }
};

#endif
