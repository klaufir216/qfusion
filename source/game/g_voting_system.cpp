#include "g_voting_system.h"
#include "g_callvotes.h"

#include "../qalgo/SingletonHolder.h"

static SingletonHolder<VotingSystem> votingSystemHolder;

void VotingSystem::Init() {
	::votingSystemHolder.Init();
}

void VotingSystem::Shutdown() {
	::votingSystemHolder.Shutdown();
}

VotingSystem *VotingSystem::Instance() {
	return ::votingSystemHolder.Instance();
}

VotingSystem::VotingSystem() {
	g_callvote_electpercentage = trap_Cvar_Get( "g_vote_percent", "55", CVAR_ARCHIVE );
	g_callvote_electtime = trap_Cvar_Get( "g_vote_electtime", "40", CVAR_ARCHIVE );
	g_callvote_enabled = trap_Cvar_Get( "g_vote_allowed", "1", CVAR_ARCHIVE );
	g_callvote_maxchanges = trap_Cvar_Get( "g_vote_maxchanges", "3", CVAR_ARCHIVE );
	g_callvote_cooldowntime = trap_Cvar_Get( "g_vote_cooldowntime", "5", CVAR_ARCHIVE );

	RegisterVote<MapVote>();
	RegisterVote<NextMapVote>();
	RegisterVote<RestartVote>();

	// wsw : pb : server admin can now disable a specific callvote command (g_disable_vote_<callvote name>)
	for( GVote *vote = votesHead; vote; vote = vote->next ) {
		trap_Cvar_Get( va( "g_disable_vote_%s", vote->name ), "0", CVAR_ARCHIVE );
	}
}

VotingSystem::~VotingSystem() {
	GVote *nextVote;
	for( GVote *vote = votesHead; vote; vote = nextVote ) {
		nextVote = vote->next;
		vote->~GVote();
		G_LevelFree( vote );
	}
}

void VotingSystem::ResetClientChoice() {
	for( ClientStatus &s: clientStatus ) {
		s.choice = 0;
		s.numChanges = 0;
	}
}

void VotingSystem::RegisterScriptVote( const char *name, const char *usage, const char *type, const char *help ) {
	if( !name ) {
		G_Printf( S_COLOR_RED "VotingSystem::RegisterScriptVote(): the name is not specified\n" );
		return;
	}

	if( FindVoteByName( name ) ) {
		G_Printf( S_COLOR_YELLOW "VotingSystem::RegisterScriptVote(): `%s` has been already registered\n", name );
		return;
	}

	void *mem = G_Malloc( sizeof( GenericScriptVote ) );
	auto *vote = new( mem )GenericScriptVote( name, help, usage, type );
	LinkVote( vote );
}

GVote *VotingSystem::FindVoteByName( const char *voteName ) {
	for( GVote *vote = votesHead; vote; vote = vote->next ) {
		if( !Q_stricmp( voteName, vote->name ) ) {
			return vote;
		}
	}
	return nullptr;
}

void VotingSystem::ResetAfterVoting() {
	if( activeVote ) {
		activeVote->ResetAfterVoting();
		activeVote = nullptr;
	}

	activeVoteTimeout = 0;
	announcementTimeout = 0;
	nextUpdateTimeout = 0;
	wasOperatorCall = false;

	for( int i = 0; i < argc; ++i ) {
		G_LevelFree( argv[i] );
		argv[i] = nullptr;
	}

	argc = std::numeric_limits<int>::min();

	trap_ConfigString( CS_ACTIVE_CALLVOTE, "" );
	trap_ConfigString( CS_ACTIVE_CALLVOTE_VOTES, "" );
}

void VotingSystem::PrintUsageTo( const edict_t *unsuccessfulCaller ) {
	G_PrintMsg( unsuccessfulCaller, "Available votes:\n" );

	for( GVote *vote = votesHead; vote; vote = vote->next ) {
		if( trap_Cvar_Value( va( "g_disable_vote_%s", vote->name ) ) ) {
			continue;
		}

		if( vote->argFormat ) {
			G_PrintMsg( unsuccessfulCaller, " %s %s\n", vote->name, vote->argFormat );
		} else {
			G_PrintMsg( unsuccessfulCaller, " %s\n", vote->name );
		}
	}
}

bool VotingSystem::ValidateActiveVote() {
	if( activeVote->CheckStatus() ) {
		return true;
	}

	int soundIndex = trap_SoundIndex( va( S_ANNOUNCER_CALLVOTE_FAILED_1_to_2, ( rand() & 1 ) + 1 ) );
	G_AnnouncerSound( nullptr, soundIndex, GS_MAX_TEAMS, true, nullptr );
	const char *format = "Vote is no longer valid\nVote %s%s%s canceled\n";
	G_PrintMsg( nullptr, format, S_COLOR_YELLOW, activeVote->String(), S_COLOR_WHITE );

	ResetAfterVoting();
	return false;
}

void VotingSystem::CheckStatus() {
	if( !activeVote ) {
		announcementTimeout = 0;
		return;
	}

	if( !ValidateActiveVote() ) {
		return;
	}

	int numVoters = 0;
	int numYesResults = 0;
	int numNoResults = 0;

	ForEachPlayer( [&]( const edict_t *ent, const gclient_t *client ) {
		const auto &status = clientStatus[PLAYERNUM( ent )];
		const auto lastActivity = client->level.last_activity;
		// ignore inactive players unless they have voted
		if( lastActivity && lastActivity + (g_inactivity_maxtime->value * 1000) < level.time && !status.choice ) {
			return;
		}

		numVoters++;
		if( status.choice > 0 ) {
			numYesResults++;
		} else if( status.choice < 0 ) {
			numNoResults++;
		}
	} );

	const int needVotes = (int)( ( numVoters * g_callvote_electpercentage->value ) / 100 );

	if( CheckPassed( numVoters, needVotes, numYesResults ) ) {
		return;
	}

	if( CheckFailed( numVoters, needVotes, numNoResults ) ) {
		return;
	}

	const auto realTime = game.realtime;
	if( announcementTimeout > realTime ) {
		return;
	}

	if( activeVoteTimeout - realTime <= 7500 && activeVoteTimeout - realTime > 2500 ) {
		G_AnnouncerSound( nullptr, trap_SoundIndex( S_ANNOUNCER_CALLVOTE_VOTE_NOW ), GS_MAX_TEAMS, true, nullptr );
	}

	const char *format =
		"Vote in progress: " S_COLOR_YELLOW "%s" S_COLOR_WHITE ", %d voted yes, %d voted no. %d required\n";

	G_PrintMsg( nullptr, format, activeVote->String(), numYesResults, numNoResults, needVotes + 1 );
	announcementTimeout = realTime + 5 * 1000;
}

bool VotingSystem::CheckPassed( int, int needVotes, int numYesResults ) {
	if( numYesResults < needVotes && !wasOperatorCall ) {
		return false;
	}

	int soundIndex = trap_SoundIndex( va( S_ANNOUNCER_CALLVOTE_PASSED_1_to_2, ( rand() & 1 ) + 1 ) );
	G_AnnouncerSound( nullptr, soundIndex, GS_MAX_TEAMS, true, nullptr );
	G_PrintMsg( nullptr, "Vote %s%s%s passed\n", S_COLOR_YELLOW, activeVote->String(), S_COLOR_WHITE );

	activeVote->Passed();
	ResetAfterVoting();
	return true;
}

bool VotingSystem::CheckFailed( int numVoters, int needVotes, int numNoResults ) {
	// no change to pass anymore
	if( !( game.realtime > activeVoteTimeout || numVoters - numNoResults <= needVotes ) ) {
		return false;
	}

	int soundIndex = trap_SoundIndex( va( S_ANNOUNCER_CALLVOTE_FAILED_1_to_2, ( rand() & 1 ) + 1 ) );
	G_AnnouncerSound( nullptr, soundIndex, GS_MAX_TEAMS, true, nullptr );
	G_PrintMsg( nullptr, "Vote %s%s%s failed\n", S_COLOR_YELLOW, activeVote->String(), S_COLOR_WHITE );

	ResetAfterVoting();
	return true;
}

void VotingSystem::HandleGiveVoteCommand( const edict_t *ent ) {
	if( !ent->r.client ) {
		return;
	}
	if( ( ent->r.svflags & SVF_FAKECLIENT ) ) {
		return;
	}

	if( !activeVote ) {
		G_PrintMsg( ent, "%sThere's no vote in progress\n", S_COLOR_RED );
		return;
	}

	const char *choiceString = trap_Cmd_Argv( 1 );
	int choice;
	if( !Q_stricmp( choiceString, "yes" ) ) {
		choice = +1;
	} else if( !Q_stricmp( choiceString, "no" ) ) {
		choice = -1;
	} else {
		const char *format = S_COLOR_RED "Invalid vote: " S_COLOR_YELLOW "%s. " S_COLOR_RED "Use yes or no\n";
		G_PrintMsg( ent, format, choiceString );
		return;
	}

	auto &status = clientStatus[PLAYERNUM( ent )];

	if( status.choice == choice ) {
		G_PrintMsg( ent, "%sYou have already voted %s\n", S_COLOR_RED, choiceString );
		return;
	}

	if( status.numChanges == g_callvote_maxchanges->integer ) {
		G_PrintMsg( ent, "%sYou cannot change your vote anymore\n", S_COLOR_RED );
		return;
	}

	status.choice = choice;
	status.numChanges++;

	CheckStatus();
}

/*
* G_CallVotes_UpdateVotesConfigString
*
* For clients that have already votes, sets and encodes
* appropriate bits in the configstring.
*/
void VotingSystem::UpdateConfigString() {
#define NUM_VOTEINTS ( ( MAX_CLIENTS + 31 ) / 32 )
	int i, n;
	int votebits[NUM_VOTEINTS];
	char cs[MAX_CONFIGSTRING_CHARS + 1];

	memset( votebits, 0, sizeof( votebits ) );
	for( i = 0; i < gs.maxclients; i++ ) {
		votebits[i >> 5] |= clientStatus[i].numChanges ? ( 1 << ( i & 31 ) ) : 0;
	}

	// find the last bitvector that isn't 0
	for( n = NUM_VOTEINTS; n > 0 && !votebits[n - 1]; n-- ) ;

	cs[0] = cs[1] = '\0';
	for( i = 0; i < n; i++ ) {
		Q_strncatz( cs, va( " %x", votebits[i] ), sizeof( cs ) );
	}
	cs[MAX_CONFIGSTRING_CHARS] = '\0';

	trap_ConfigString( CS_ACTIVE_CALLVOTE_VOTES, cs + 1 );
}

void VotingSystem::Frame() {
	if( !activeVote ) {
		nextUpdateTimeout = 0;
		return;
	}

	const auto realTime = game.realtime;
	if( nextUpdateTimeout > realTime ) {
		return;
	}

	// TODO: Do this only on demand
	UpdateConfigString();

	CheckStatus();
	nextUpdateTimeout = realTime + 1000;
}

bool VotingSystem::CheckCanVoteNow( const edict_t *caller ) {
	if( caller->r.client->isoperator ) {
		return true;
	}

	// Check voting timeout for the client
	auto lastVotedAt = clientStatus[PLAYERNUM( caller )].lastVotedAt;
	if( lastVotedAt && lastVotedAt + g_callvote_cooldowntime->integer * 1000 < game.realtime ) {
		G_PrintMsg( caller, "%sYou can not call a vote right now\n", S_COLOR_RED );
		return false;
	}

	if( caller->s.team != TEAM_SPECTATOR ) {
		return true;
	}

	const auto matchState = GS_MatchState();
	if( matchState != MATCH_STATE_PLAYTIME && matchState != MATCH_STATE_COUNTDOWN ) {
		return true;
	}

	if( GS_MatchPaused() ) {
		return true;
	}

	// Find somebody playing
	const auto *__restrict edicts = game.edicts;
	for( int team = TEAM_ALPHA; team < GS_MAX_TEAMS; ++team ) {
		const auto &__restrict list = ::teamlist[team];
		for( int i = 0; i < list.numplayers; ++i ) {
			const edict_t *e = edicts + list.playerIndices[i];
			if( !e->r.inuse ) {
				continue;
			}
			if( e->r.svflags & SVF_FAKECLIENT ) {
				continue;
			}
			G_PrintMsg( caller, "%sSpectators cannot start a vote while a match is in progress\n", S_COLOR_RED );
			return false;
		}
	}

	return true;
}

void VotingSystem::TryStartVote( const edict_t *caller, bool isOperatorCall ) {
	const char *votename;
	callvotetype_t *callvote;

	if( !g_callvote_enabled->integer ) {
		G_PrintMsg( caller, "%sCallvoting is disabled on this server\n", S_COLOR_RED );
		return;
	}

	if( activeVote ) {
		G_PrintMsg( caller, "%sA vote is already in progress\n", S_COLOR_RED );
		return;
	}

	if( !isOperatorCall && CheckCanVoteNow( caller ) ) {
		return;
	}

	votename = trap_Cmd_Argv( 1 );
	if( !votename || !votename[0] ) {
		G_CallVotes_PrintUsagesToPlayer( caller );
		return;
	}

	if( strlen( votename ) > MAX_QPATH ) {
		G_PrintMsg( ent, "%sInvalid vote\n", S_COLOR_RED );
		G_CallVotes_PrintUsagesToPlayer( caller );
		return;
	}

	//find the actual callvote command
	for( callvote = callvotesHeadNode; callvote != NULL; callvote = callvote->next ) {
		if( callvote->name && !Q_stricmp( callvote->name, votename ) ) {
			break;
		}
	}

	//unrecognized callvote type
	if( callvote == NULL ) {
		G_PrintMsg( ent, "%sUnrecognized vote: %s\n", S_COLOR_RED, votename );
		G_CallVotes_PrintUsagesToPlayer( ent );
		return;
	}

	// wsw : pb : server admin can now disable a specific vote command (g_disable_vote_<vote name>)
	// check if vote is disabled
	if( !isopcall && trap_Cvar_Value( va( "g_disable_vote_%s", callvote->name ) ) ) {
		G_PrintMsg( ent, "%sCallvote %s is disabled on this server\n", S_COLOR_RED, callvote->name );
		return;
	}

	// allow a second cvar specific for opcall
	if( isopcall && trap_Cvar_Value( va( "g_disable_opcall_%s", callvote->name ) ) ) {
		G_PrintMsg( ent, "%sOpcall %s is disabled on this server\n", S_COLOR_RED, callvote->name );
		return;
	}

	//we got a valid type. Get the parameters if any
	if( callvote->expectedargs != trap_Cmd_Argc() - 2 ) {
		if( callvote->expectedargs != -1 &&
			( callvote->expectedargs != -2 || trap_Cmd_Argc() - 2 > 0 ) ) {
			// wrong number of parametres
			G_CallVotes_PrintHelpToPlayer( ent, callvote );
			return;
		}
	}

	callvoteState.vote.argc = trap_Cmd_Argc() - 2;
	for( i = 0; i < callvoteState.vote.argc; i++ )
		callvoteState.vote.argv[i] = G_CopyString( trap_Cmd_Argv( i + 2 ) );

	callvoteState.vote.callvote = callvote;
	callvoteState.vote.caller = ent;
	callvoteState.vote.operatorcall = isopcall;

	//validate if there's a validation func
	if( callvote->validate != NULL && !callvote->validate( &callvoteState.vote, true ) ) {
		G_CallVotes_PrintHelpToPlayer( ent, callvote );
		G_CallVotes_Reset(); // free the args
		return;
	}

	//we're done. Proceed launching the election
	for( i = 0; i < gs.maxclients; i++ )
		G_CallVotes_ResetClient( i );

	callvoteState.timeout = game.realtime + ( g_callvote_electtime->integer * 1000 );

	//caller is assumed to vote YES
	clientVoted[PLAYERNUM( ent )] = VOTED_YES;
	clientVoteChanges[PLAYERNUM( ent )]--;

	ent->r.client->level.callvote_when = callvoteState.timeout;

	trap_ConfigString( CS_ACTIVE_CALLVOTE, G_CallVotes_String( &callvoteState.vote ) );

	G_AnnouncerSound( NULL, trap_SoundIndex( va( S_ANNOUNCER_CALLVOTE_CALLED_1_to_2, ( rand() & 1 ) + 1 ) ), GS_MAX_TEAMS, true, NULL );

	G_PrintMsg( NULL, "%s" S_COLOR_WHITE " requested to vote " S_COLOR_YELLOW "%s\n",
				ent->r.client->netname, G_CallVotes_String( &callvoteState.vote ) );

	G_PrintMsg( NULL, "Press " S_COLOR_YELLOW "F1" S_COLOR_WHITE " to " S_COLOR_YELLOW "vote yes"
					  S_COLOR_WHITE " or " S_COLOR_YELLOW "F2" S_COLOR_WHITE " to " S_COLOR_YELLOW "vote no"
					  S_COLOR_WHITE ", or cast your vote using the " S_COLOR_YELLOW "in-game menu\n" );

	G_CallVotes_Think(); // make the first think
}

void VotingSystem::HandleCallVoteCommand( const edict_t *caller ) {
	if( ( caller->r.svflags & SVF_FAKECLIENT ) ) {
		return;
	}
	TryStartVote( caller, false );
}

void VotingSystem::TryToPassOrCancelVote( const edict_t *caller, int forceVote ) {
	assert( caller->r.client->isoperator );

	if( !activeVote ) {
		G_PrintMsg( caller, "There's no callvote to %s.\n", forceVote < 0 ? "cancel" : "pass" );
		return;
	}

	for( edict_t *other = game.edicts + 1; PLAYERNUM( other ) < gs.maxclients; other++ ) {
		if( !other->r.inuse || trap_GetClientState( PLAYERNUM( other ) ) < CS_SPAWNED ) {
			continue;
		}
		if( ( other->r.svflags & SVF_FAKECLIENT ) ) {
			continue;
		}

		clientStatus[PLAYERNUM( other )].choice = forceVote;
	}

	const char *action = forceVote < 0 ? "cancelled" : "passed";
	G_PrintMsg( nullptr, "Callvote has been %s by %s\n", action, caller->r.client->netname );
}

void VotingSystem::TryMovingPlayerToTeam( const edict_t *caller ) {
	assert( caller->r.client->isoperator );

	char *splayer = trap_Cmd_Argv( 2 );
	char *steam = trap_Cmd_Argv( 3 );
	edict_t *playerEnt;
	int newTeam;

	if( !steam || !steam[0] || !splayer || !splayer[0] ) {
		G_PrintMsg( caller, "Usage 'putteam <player id > <team name>'.\n" );
		return;
	}

	if( ( newTeam = GS_Teams_TeamFromName( steam ) ) < 0 ) {
		G_PrintMsg( caller, "The team '%s' doesn't exist.\n", steam );
		return;
	}

	if( !( playerEnt = G_PlayerForText( splayer ) ) ) {
		G_PrintMsg( caller, "The player '%s' couldn't be found.\n", splayer );
		return;
	}

	const char *playerName = playerEnt->r.client->netname;
	const char *teamName = GS_TeamName( newTeam );

	if( playerEnt->s.team == newTeam ) {
		G_PrintMsg( caller, "The player '%s' is already in team '%s'.\n", playerName, teamName );
		return;
	}

	G_Teams_SetTeam( playerEnt, newTeam );
	G_PrintMsg( nullptr, "%s was moved to team %s by %s.\n", playerName, teamName, caller->r.client->netname );
}

void VotingSystem::HandleOpCallCommand( const edict_t *caller ) {
	if( !caller->r.client ) {
		return;
	}

	if( ( caller->r.svflags & SVF_FAKECLIENT ) ) {
		return;
	}

	if( !caller->r.client->isoperator ) {
		G_PrintMsg( caller, "You are not a game operator\n" );
		return;
	}

	if( !Q_stricmp( trap_Cmd_Argv( 1 ), "help" ) ) {
		G_PrintMsg( caller, "Opcall can be used with all callvotes and the following commands:\n" );
		G_PrintMsg( caller, "-help\n - passvote\n- cancelvote\n- putteam\n" );
		return;
	}

	if( !Q_stricmp( trap_Cmd_Argv( 1 ), "cancelvote" ) ) {
		TryToPassOrCancelVote( caller, -1 );
		return;
	}

	if( !Q_stricmp( trap_Cmd_Argv( 1 ), "passvote" ) ) {
		TryToPassOrCancelVote( caller, +1 );
		return;
	}

	if( !Q_stricmp( trap_Cmd_Argv( 1 ), "putteam" ) ) {
		TryMovingPlayerToTeam( caller );
		return;
	}

	TryStartVote( caller, true );
}

GWebResponse VotingSystem::ServeWebRequest( GWebRequest *request ) {
	if( request->method != HTTP_METHOD_GET && request->method != HTTP_METHOD_HEAD ) {
		return GWebResponse::BadRequest();
	}

	if( !Q_strnicmp( request->resource, "callvotes/", 10 ) ) {
		return ServeListOfVotesRequest( request );
	}

	if( !Q_strnicmp( request->resource, "callvote/", 9 ) ) {
		return ServeVoteArgsRequest( request );
	}

	return GWebResponse::NotFound();
}

GWebResponse VotingSystem::ServeListOfVotesRequest( GWebRequest *request ) {
	char *msg = nullptr;
	size_t len = 0, size = 0;

	constexpr const char *format =
		"{\n"
		"\"name\"" " " "\"%s\"" "\n"
		"\"expected_args\"" " " "\"%d\"" "\n"
		"\"argument_format\"" " " "\"%s\"" "\n"
		"\"argument_type\"" " " "\"%s\"" "\n"
		"\"help\"" " " "\"%s\"" "\n"
		"}\n";

	// print the list of callvotes
	for( GVote *vote = votesHead; vote; vote = vote->next ) {
		if( trap_Cvar_Value( va( "g_disable_vote_%s", vote->name ) ) ) {
			continue;
		}

		const char *argFormat = vote->argFormat ? vote->argFormat : "";
		const char *argType = vote->argType ? vote->argType : "string";
		const char *help = vote->briefHelp ? vote->briefHelp : "";
		G_AppendString( &msg, va( format, vote->name, vote->numExpectedArgs, argFormat, argType, help ), &len, &size );
	}

	return GWebResponse::Ok( msg, len );
}

GWebResponse VotingSystem::ServeVoteArgsRequest( GWebRequest *request ) {
	const char *voteName = request->resource + 9;
	if( GVote *vote = FindVoteByName( voteName ) ) {
		return vote->ServeWebRequest( request );
	}

	return GWebResponse::NotFound();
}