#ifndef QFUSION_G_CALLVOTES_H
#define QFUSION_G_CALLVOTES_H

#include "g_local.h"

#include <algorithm>
#include <functional>
#include <memory>



class GVote {
	friend class VotingSystem;
protected:
	static constexpr const char *FMT_INTEGER = "<number>";
	static constexpr const char *ARG_INTEGER = "integer";

	static constexpr const char *FMT_MINUTES = "<minutes>";
	static constexpr const char *ARG_MINUTES = "integer";

	static constexpr const char *FMT_PLAYER = "<player>";
	static constexpr const char *ARG_PLAYER = "option";

	static constexpr const char *FMT_BOOL = "<1 or 0>";
	static constexpr const char *ARG_BOOL = "bool";

	int numExpectedArgs;
	const char *name;
	const char *briefHelp;
	const char *argFormat;
	const char *argType;

	VotingSystem *parent { nullptr };
	GVote *next { nullptr };

	const edict_t *Caller() const { return parent->caller; }
	int Argc() const { return parent->Argc(); }
	char *Argv( int argNum ) { return parent->Argv( argNum ); }
	const char *Argv( int argNum ) const { return parent->Argv( argNum ); }

	// TODO: std::optional once we can use C++17
	bool GetArgAsInt( int argNum, int *result );

	explicit GVote( const char *name_, const char *briefHelp_, int numExpectedArgs_,
					const char *argFormat_ = nullptr, const char *argType_ = nullptr )
		: numExpectedArgs( numExpectedArgs_ )
		, name( name_ ), briefHelp( briefHelp_ ), argFormat( argFormat_ ), argType( argType_ ) {}

	virtual ~GVote() = default;
public:
	virtual bool TryActivate() = 0;
	virtual bool CheckStatus() = 0;

	virtual void Passed() = 0;
	virtual const char *Current() { return ""; };
	virtual const char *String() = 0;

	virtual void ResetAfterVoting();

	virtual GWebResponse ServeWebRequest( GWebRequest *request ) {
		return GWebResponse::NotFound();
	};

	virtual void PrintHelpTo( const edict_t *player );
};

class MapVote final : public GVote {
public:
	MapVote(): GVote( "map", "Changes map", 1 ) {}

	bool TryActivate() override;

	bool CheckStatus() override {
		// A map cannot become invalid once a server has started
		return true;
	}

	void Passed() override {
		Q_strncpyz( level.forcemap, Q_strlwr( Argv( 0 ) ), sizeof( level.forcemap ) );
		G_EndMatch();
	}

	const char *Current() override {
		return level.mapname;
	}

	GWebResponse ServeWebRequest( GWebRequest *request ) override;

	void PrintHelpToPlayer( const edict_t *caller ) override;
};

class NextMapVote final : public GVote {
public:
	NextMapVote(): GVote( "nextmap", "Jumps to the next map", 0 ) {}

	bool TryActivate() override { return true; }

	void Passed() override {
		level.forcemap[0] = 0;
		G_EndMatch();
	}
};

class RestartVote final : public GVote {
public:
	RestartVote(): GVote( "restart", "Restarts current map", 0 ) {}

	bool TryActivate() override { return true; }

	void Passed() override {
		G_RestartLevel();
	}
};

class ScoreLimitVote final : public GVote {
	static constexpr const char *HELP =
		"Sets the number of frags or caps needed to win the match\nSpecify 0 to disable";
public:
	ScoreLimitVote(): GVote( "scorelimit", HELP, 1, "<number>", "integer" ) {}

	void Passed() override;
};

class TimeLimitVote : public GVote {
	static constexpr const char *HELP =
		"Sets number of minutes after which the match ends\nSpecify 0 to disable";
public:
	TimeLimitVote(): GVote( "timelimit", HELP, 1, "<minutes>", "integer" ) {}

	void Passed() override;
};

class GametypeVote : public GVote {
public:
	GametypeVote(): GVote( "gametype", "Changes the gametype", 1, "<name>", "option" ) {}

	void Passed() override;
};

class WarmupTimeLimitVote : public GVote {
	static constexpr const char *HELP =
		"Sets the number of minutes after which the warmup ends\nSpecify 0 to disable";
public:
	WarmupTimeLimitVote(): GVote( "warmup_timelimit", HELP, 1, "<minutes>", "integer" ) {}

	void Passed() override;
};

class ExtendedTimeVote : public GVote {
	static constexpr const char *HELP =
		"Sets the length of the overtime\nSpecify 0 to enable sudden death mode";
public:
	ExtendedTimeVote(): GVote( "extended_time", HELP, 1, "<minutes>", "integer" ) {}

	void Passed() override;
};

class MaxTeamPlayersVote : public GVote {
	static constexpr const char *HELP = "Sets the maximum number of players in one team";
public:
	MaxTeamPlayersVote(): GVote( "maxteamplayers", HELP, 1, "<number>", "integer" ) {}
};

class LockVote : public GVote {
	static constexpr const char *HELP = "Locks teams to disallow players joining in mid-game";
public:
	LockVote(): GVote( "lock", HELP, 0 ) {}
	void Passed() override;
};

class UnlockVote : public GVote {
	static constexpr const char *HELP = "Unlocks teams to allow players joining in mid-game";
public:
	UnlockVote(): GVote( "unlock", HELP, 0 ) {};
	void Passed() override;
};

class AllReadyVote : public GVote {
	static constexpr const char *HELP = "Sets all players as ready so the match can start";
public:
	AllReadyVote(): GVote( "allready", HELP, 0 ) {}
	bool CheckStatus() override;
	void Passed() override;
};

class KickLikeVote: public GVote {
	char playerNick[MAX_NAME_BYTES];
	int playerId { -1 };
public:
	KickLikeVote( const char *name_, const char *help_ )
		: GVote( name_, help_, 1, FMT_PLAYER, ARG_PLAYER ) {}

	bool TryActivate() override;
	bool CheckStatus() override;
};

class RemoveVote : public KickLikeVote {
	static constexpr const char *HELP = "Forces player back to spectator mode";
public:
	RemoveVote(): KickLikeVote( "remove", HELP ) {}
	void Passed() override;
};

class KickVote : public KickLikeVote {
	static constexpr const char *HELP = "Removes player from the server";
public:
	KickVote(): KickLikeVote( "kick", HELP ) {}
	void Passed() override;
};

class KickBanVote : public KickLikeVote {
	static constexpr const char *HELP = "Removes player from the server and bans his IP-address for 15 minutes";
public:
	KickBanVote(): KickLikeVote( "kickban", HELP ) {}
	void Passed() override;
};

class MuteVote : public KickLikeVote {
	static constexpr const char *HELP =  "Disallows chat messages from the muted player";
public:
	MuteVote(): KickLikeVote( "mute", HELP ) {}
	void Passed() override;
};

class UnMuteVote : public KickLikeVote {
	static constexpr const char *HELP = "Reallows chat messages from the unmuted player";
public:
	UnMuteVote(): KickLikeVote( "unmute", HELP ) {}
	void Passed() override;
};

class SetIndividualAntiWallHackVote : public KickLikeVote {
	static constexpr const char *HELP = "";
public:
	SetIndividualAntiWallHackVote(): KickLikeVote( "set_antiwallhack_for", HELP ) {}
};

class SetIndividualAntiRadarVote : public KickLikeVote {
	static constexpr const char *HELP = "";
public:
	SetIndividualAntiRadarVote(): KickLikeVote( "set_antiwallhack_for", HELP ) {}
};

class SetIndividualAntiCheatVote : public KickLikeVote {
	static constexpr const char *HELP = "";
public:
	SetIndividualAntiCheatVote(): KickLikeVote( "set_anticheat_for", HELP ) {}
};

class ResetIndividualAntiCheatVote : public KickLikeVote {
	static constexpr const char *HELP = "";
public:
	ResetIndividualAntiCheatVote(): KickLikeVote( "reset_anticheat_for", HELP ) {}
};

class EnableGlobalAntiWallHackVote : public GVote {};
class EnableGlobalAntiRadarVote : public GVote {};
class EnableGlobalAntiCheatVote : public GVote {};

class NumBotsVote : public GVote {
	static constexpr const char *HELP = "";
public:
	NumBotsVote(): GVote( "numbots", HELP, -1 ) {}
};

class AllowTeamDamageVote : public GVote {

};

class AllowSelfDamageVote : public GVote {

};

class AllowFallDamageVote : public GVote {

};

class AllowInstaJumpVote : public GVote {

};

class AllowInstaShieldVote : public GVote {

};

class TimeoutVote : public GVote {
public:
	TimeoutVote(): GVote( "timeout", "Pauses the game", 0 ) {}
};

class TimeInVote : public GVote {
public:
	TimeInVote(): GVote( "timein", "Resumes the game if in timeout", 0 ) {}
};

class AllowUnevenVote : public GVote {};

class ShuffleVote : public GVote {
public:
	ShuffleVote(): GVote( "shuffle", "Shuffles teams", 0 ) {}
};

class ReBalanceVote : public GVote {
public:
	ReBalanceVote(): GVote( "rebalance", "Balances teams", 0 ) {}
};

class GenericScriptVote : public GVote {
	char *nameStorage { nullptr };
	char *helpStorage { nullptr };
	char *formatStorage { nullptr };
	char *typeStorage { nullptr };
public:
	GenericScriptVote( const char *name_, const char *briefHelp_,
					   const char *argFormat_, const char *argType_ );

	~GenericScriptVote() override;

	bool TryActivate() override;
	bool CheckStatus() override;
	void Passed() override;
};

inline void VotingSystem::LinkVote( GVote *vote ) {
	vote->next = votesHead;
	votesHead = vote;
}

#endif
