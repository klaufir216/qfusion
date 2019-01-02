#ifndef QFUSION_G_VOTING_SYSTEM_H
#define QFUSION_G_VOTING_SYSTEM_H

#include "g_local.h"

class GVote;

class VotingSystem {
	friend class GVote;

	cvar_t *g_callvote_electpercentage;
	cvar_t *g_callvote_electtime;          // in seconds
	cvar_t *g_callvote_enabled;
	cvar_t *g_callvote_maxchanges;
	cvar_t *g_callvote_cooldowntime;

	GVote *votesHead { nullptr };
	GVote *activeVote { nullptr };

	int64_t activeVoteTimeout { 0 };
	int64_t announcementTimeout { 0 };
	int64_t nextUpdateTimeout { 0 };

	const edict_t *caller { nullptr };
	int argc { 0 };
	char *argv[MAX_STRING_TOKENS];
	bool wasOperatorCall { false };

	struct ClientStatus {
		int64_t lastVotedAt { 0 };
		int choice { 0 };
		int numChanges { 0 };
	};

	ClientStatus clientStatus[MAX_CLIENTS];

	void ResetClientChoice();

	class GVote *FindVoteByName( const char *voteName );

	template <typename V> void RegisterVote() {
		void *mem = G_LevelMalloc( sizeof( V ) );
		V *vote = new( mem )V;
		LinkVote( vote );
	}

	inline void LinkVote( GVote *vote );

	VotingSystem();
	~VotingSystem();

	const edict_t *Caller() const { return caller; }

	int Argc() const { return argc; }

	char *Argv( int argNum ) {
		assert( (unsigned)argNum < (unsigned)argc );
		return argv[argNum];
	}

	const char *Argv( int argNum ) const {
		assert( (unsigned)argNum < (unsigned)argc );
		return argv[argNum];
	}

	void ResetAfterVoting();

	void UpdateConfigString();
	void CheckStatus();
	bool ValidateActiveVote();

	bool CheckPassed( int numVoters, int needVotes, int numYesResults );
	bool CheckFailed( int numVoters, int needVotes, int numNoResults );

	bool CheckCanVoteNow( const edict_t *caller );

	void TryStartVote( const edict_t *caller, bool calledByOperator );
	void PrintUsageTo( const edict_t *unsuccessfulCaller );

	void TryToPassOrCancelVote( const edict_t *caller, int forceVote );
	void TryMovingPlayerToTeam( const edict_t *caller );
public:
	static void Init();
	static void Shutdown();
	static VotingSystem *Instance();

	void Frame();

	GWebResponse ServeWebRequest( GWebRequest *request );
	GWebResponse ServeListOfVotesRequest( GWebRequest *request );
	GWebResponse ServeVoteArgsRequest( GWebRequest *request );

	void RegisterScriptVote( const char *name, const char *usage, const char *type, const char *help );

	void HandleCallVoteCommand( const edict_t *caller );
	void HandleOpCallCommand( const edict_t *caller );
	void HandleGiveVoteCommand( const edict_t *caller );
};

#endif
