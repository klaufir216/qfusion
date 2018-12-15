/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include "g_local.h"
#include "../matchmaker/mm_query.h"
#include "g_gametypes.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "../qalgo/SingletonHolder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <functional>
#include <new>
#include <utility>
#include <thread>

static SingletonHolder<StatsowFacade> statsHolder;

// number of raceruns to send in one batch
#define RACERUN_BATCH_SIZE  16

//====================================================

static clientRating_t *g_ratingAlloc( const char *gametype, float rating, float deviation, mm_uuid_t uuid ) {
	auto *cr = (clientRating_t*)G_Malloc( sizeof( clientRating_t ) );
	if( !cr ) {
		return nullptr;
	}

	Q_strncpyz( cr->gametype, gametype, sizeof( cr->gametype ) - 1 );
	cr->rating = rating;
	cr->deviation = deviation;
	cr->next = nullptr;
	cr->uuid = uuid;

	return cr;
}

static clientRating_t *g_ratingCopy( const clientRating_t *other ) {
	return g_ratingAlloc( other->gametype, other->rating, other->deviation, other->uuid );
}

// free the list of clientRatings
static void g_ratingsFree( clientRating_t *list ) {
	clientRating_t *next;

	while( list ) {
		next = list->next;
		G_Free( list );
		list = next;
	}
}

void StatsowFacade::UpdateAverageRating() {
	clientRating_t avg;

	if( !ratingsHead ) {
		avg.rating = MM_RATING_DEFAULT;
		avg.deviation = MM_DEVIATION_DEFAULT;
	} else {
		Rating_AverageRating( &avg, ratingsHead );
	}

	// Com_Printf("g_serverRating: Updated server's skillrating to %f\n", avg.rating );

	trap_Cvar_ForceSet( "sv_skillRating", va( "%.0f", avg.rating ) );
}

void StatsowFacade::TransferRatings() {
	clientRating_t *cr, *found;
	edict_t *ent;
	gclient_t *client;

	// shuffle the ratings back from game.ratings to clients->ratings and back
	// based on current gametype
	g_ratingsFree( ratingsHead );
	ratingsHead = nullptr;

	for( ent = game.edicts + 1; PLAYERNUM( ent ) < gs.maxclients; ent++ ) {
		client = ent->r.client;

		if( !client ) {
			continue;
		}
		if( !ent->r.inuse ) {
			continue;
		}

		// temphack for duplicate client entries
		found = Rating_FindId( ratingsHead, client->mm_session );
		if( found ) {
			continue;
		}

		found = Rating_Find( client->ratings, gs.gametypeName );

		// create a new default rating if this doesnt exist
		// DONT USE G_AddDefaultRating cause this will cause double entries in game.ratings
		if( !found ) {
			found = g_ratingAlloc( gs.gametypeName, MM_RATING_DEFAULT, MM_DEVIATION_DEFAULT, client->mm_session );
			if( !found ) {
				continue;   // ??

			}
			found->next = client->ratings;
			client->ratings = found;
		}

		// add it to the games list
		cr = g_ratingCopy( found );
		cr->next = ratingsHead;
		ratingsHead = cr;
	}

	UpdateAverageRating();
}

// This doesnt update ratings, only inserts new default rating if it doesnt exist
// if gametype is NULL, use current gametype
clientRating_t *StatsowFacade::AddDefaultRating( edict_t *ent, const char *gametype ) {
	if( !gametype ) {
		gametype = gs.gametypeName;
	}

	auto *client = ent->r.client;
	if( !ent->r.inuse ) {
		return nullptr;
	}

	auto *cr = Rating_Find( client->ratings, gametype );
	if( !cr ) {
		cr = g_ratingAlloc( gametype, MM_RATING_DEFAULT, MM_DEVIATION_DEFAULT, ent->r.client->mm_session );
		if( !cr ) {
			return nullptr;
		}

		cr->next = client->ratings;
		client->ratings = cr;
	}

	TryUpdatingGametypeRating( client, cr, gametype );
	return cr;
}

// this inserts a new one, or updates the ratings if it exists
clientRating_t *StatsowFacade::AddRating( edict_t *ent, const char *gametype, float rating, float deviation ) {
	if( !gametype ) {
		gametype = gs.gametypeName;
	}

	auto *client = ent->r.client;
	if( !ent->r.inuse ) {
		return nullptr;
	}

	auto *cr = Rating_Find( client->ratings, gametype );
	if( cr ) {
		cr->rating = rating;
		cr->deviation = deviation;
	} else {
		cr = g_ratingAlloc( gametype, rating, deviation, ent->r.client->mm_session );
		if( !cr ) {
			return nullptr;
		}

		cr->next = client->ratings;
		client->ratings = cr;
	}

	TryUpdatingGametypeRating( client, cr, gametype );
	return cr;
}

void StatsowFacade::TryUpdatingGametypeRating( const gclient_t *client,
											   const clientRating_t *addedRating,
											   const char *addedForGametype ) {
	// If the gametype is not a current gametype
	if( strcmp( addedForGametype, gs.gametypeName ) != 0 ) {
		return;
	}

	// add this rating to current game-ratings
	auto *found = Rating_FindId( ratingsHead, client->mm_session );
	if( !found ) {
		found = g_ratingCopy( addedRating );
		if( found ) {
			found->next = ratingsHead;
			ratingsHead = found;
		}
	} else {
		// update values
		found->rating = addedRating->rating;
		found->deviation = addedRating->deviation;
	}

	UpdateAverageRating();
}

// removes all references for given entity
void StatsowFacade::RemoveRating( edict_t *ent ) {
	gclient_t *client;
	clientRating_t *cr;

	client = ent->r.client;

	// first from the game
	cr = Rating_DetachId( &ratingsHead, client->mm_session );
	if( cr ) {
		G_Free( cr );
	}

	// then the clients own list
	g_ratingsFree( client->ratings );
	client->ratings = nullptr;

	UpdateAverageRating();
}

// from AS
raceRun_t *StatsowFacade::NewRaceRun( const edict_t *ent, int numSectors ) {
	gclient_t *cl;
	raceRun_t *rr;

	cl = ent->r.client;

	if( !ent->r.inuse || !cl  ) {
		return nullptr;
	}

	rr = &cl->level.stats.currentRun;
	if( rr->times ) {
		G_LevelFree( rr->times );
	}

	rr->times = ( int64_t * )G_LevelMalloc( ( numSectors + 1 ) * sizeof( *rr->times ) );
	rr->numSectors = numSectors;
	rr->owner = cl->mm_session;
	if( cl->mm_session.IsValidSessionId() ) {
		rr->nickname[0] = '\0';
	} else {
		Q_strncpyz( rr->nickname, cl->netname, MAX_NAME_BYTES );
	}

	return rr;
}

// from AS
void StatsowFacade::SetRaceTime( edict_t *owner, int sector, int64_t time ) {
	auto *const cl = owner->r.client;

	if( !owner->r.inuse || !cl ) {
		return;
	}

	raceRun_t *const rr = &cl->level.stats.currentRun;
	if( sector < -1 || sector >= rr->numSectors ) {
		return;
	}

	// normal sector
	if( sector >= 0 ) {
		rr->times[sector] = time;
	} else if( rr->numSectors > 0 ) {
		raceRun_t *nrr; // new global racerun

		rr->times[rr->numSectors] = time;
		rr->utcTimestamp = game.utcTimeMillis;

		// validate the client
		// no bots for race, at all
		if( owner->r.svflags & SVF_FAKECLIENT /* && mm_debug_reportbots->value == 0 */ ) {
			G_Printf( "G_SetRaceTime: not reporting fakeclients\n" );
			return;
		}

		// Note: the test whether client session id has been removed,
		// race runs are reported for non-authenticated players too

		// push new run
		nrr = raceRuns.New();
		memcpy( nrr, rr, sizeof( raceRun_t ) );

		// reuse this one in nrr
		rr->times = nullptr;

		// see if we have to push intermediate result
		// TODO: We can live with eventual consistency of race records, but it should be kept in mind
		// TODO: Send new race runs every N seconds, or if its likely to be a new record
		if( raceRuns.size() >= RACERUN_BATCH_SIZE ) {
			// Update an actual nickname that is going to be used to identify a run for a non-authenticated player
			if( !cl->mm_session.IsValidSessionId() ) {
				Q_strncpyz( rr->nickname, cl->netname, MAX_NAME_BYTES );
			}
			SendReport();

			// double-check this for memory-leaks
			raceRuns.Clear();
		}
	}

	// Update an actual nickname that is going to be used to identify a run for a non-authenticated player
	if( !cl->mm_session.IsValidSessionId() ) {
		Q_strncpyz( rr->nickname, cl->netname, MAX_NAME_BYTES );
	}
}

int StatsowFacade::ForEachRaceRun( const std::function<void( const raceRun_t & )> &applyThis ) const {
	for( const auto &run: raceRuns ) {
		applyThis( run );
	}

	return (int)raceRuns.size();
}

static SingletonHolder<StatsowFacade> statsInstanceHolder;

void StatsowFacade::Init() {
	statsInstanceHolder.Init();
}

void StatsowFacade::Shutdown() {
	statsInstanceHolder.Shutdown();
}

StatsowFacade *StatsowFacade::Instance() {
	return statsInstanceHolder.Instance();
}

StatsowFacade::~StatsowFacade() {
	if( activeQuery ) {
		bool succeeded = WaitForQuery();
		trap_MM_DeleteQuery( activeQuery );
		if( succeeded ) {
			return;
		}

		if( raceRunsToSend.empty() ) {
			return;
		}

		raceRuns.MergeWith( std::move( raceRunsToSend ) );
		hasPendingResults = true;
	}

	if( !hasPendingResults ) {
		return;
	}

	SendRaceReport();
	if( activeQuery ) {
		(void)WaitForQuery();
		trap_MM_DeleteQuery( activeQuery );
	}
}

bool StatsowFacade::WaitForQuery() {
	assert( activeQuery );

	// This is ugly af but it helps to avoid intrusive changes to the
	int numAttepts = 0;
	for(;; ) {
		trap_MM_SendQuery( activeQuery );

		std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
		while( !activeQuery->IsReady() ) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 16 ) );
		}

		if( activeQuery->HasSucceeded() ) {
			G_Printf( "The query we were waiting for has been sent successfully\n" );
			return true;
		}

		const char *const tag = "StatsowFacade::WaitForQuery()";
		if( !activeQuery->ShouldRetry() ) {
			G_Printf( S_COLOR_RED "%s: Unrecoverable error. Query data is lost\n", tag );
			return false;
		}
		if( numAttepts == 2 ) {
			G_Printf( S_COLOR_RED "%s: Stopping fruitless retry attempt. Query data is lost\n", tag );
			return false;
		}

		numAttepts++;
		activeQuery->ResetForRetry();
		std::this_thread::sleep_for( std::chrono::milliseconds( 250 + 250 * numAttepts ) );
	}
}

void StatsowFacade::Frame() {
	if( !activeQuery ) {
		if( hasPendingResults && !raceRuns.empty() ) {
			// Start sending a race report for pending results
			SendRaceReport();
			hasPendingResults = false;
		}
		return;
	}

	if( !activeQuery->IsReady() ) {
		return;
	}

	if( activeQuery->HasSucceeded() ) {
		trap_MM_DeleteQuery( activeQuery );
		activeQuery = nullptr;
		// We're going to check for pending results next frame
		return;
	}

	if( activeQuery->ShouldRetry() ) {
		activeQuery->ResetForRetry();
		trap_MM_SendQuery( activeQuery );
		return;
	}

	trap_MM_DeleteQuery( activeQuery );
	// There should currently be some race runs to deliver
	assert( !raceRunsToSend.empty() );
	// Add non-delivered race runs to the accumulated ones
	raceRuns.MergeWith( std::move( raceRunsToSend ) );
	hasPendingResults = true;
}

void StatsowFacade::ClearEntries() {
	ClientEntry *next;
	for( ClientEntry *e = clientEntriesHead; e; e = next ) {
		next = e->next;
		e->~ClientEntry();
		G_Free( e );
	}

	clientEntriesHead = nullptr;
}

void StatsowFacade::OnClientHadPlaytime( const gclient_t *client ) {
	if( isDiscarded ) {
		return;
	}

	if( !sv_mm_enable->integer ) {
		isDiscarded = true;
		return;
	}

	// TODO: Did we forget something else? What if sv_mm_enable is true but the session is not valid?
	if( !GS_MMCompatible() ) {
		isDiscarded = true;
		return;
	}

	if( GS_RaceGametype() ) {
		return;
	}

	const char *reason = nullptr;
	const edict_t *ent = game.edicts + ENTNUM( client );
	// Check whether it's a bot first (they do not have valid session ids as well)
	if( ent->ai ) {
		if( AI_GetType( ent->ai ) == AI_ISBOT ) {
			reason = "A bot had a play-time";
		}
		// The report is still valid if it's an AI but not a bot.
		// TODO: Are logged frags valid as well in this case?
	} else {
		if( !client->mm_session.IsValidSessionId() ) {
			reason = va( "An anonymous player `%s` had a play-time", client->netname );
		}
	}

	if( !reason ) {
		return;
	}

	// Print to everybody
	G_PrintMsg( nullptr, S_COLOR_YELLOW "%s. Discarding match report...\n", reason );

	// Do not hold no longer useful data
	ClearEntries();
	// TODO:!!!!!!!!
	// TODO:!!!!!!!!
	// TODO:!!!!!!!! clear other data as well

	isDiscarded = true;
}

void StatsowFacade::OnClientDisconnected( edict_t *ent ) {
	if( GS_RaceGametype() ) {
		// Force sending race reports if somebody disconnects
		SendReport();
		return;
	}

	if( ent->r.client->team == TEAM_SPECTATOR ) {
		return;
	}

	const bool isMatchOver = GS_MatchState() == MATCH_STATE_POSTMATCH;
	// If not in match-time and not in post-match ignore this
	if( !isMatchOver && ( GS_MatchState() != MATCH_STATE_PLAYTIME ) ) {
		return;
	}

	ChatHandlersChain::Instance()->OnClientDisconnected( ent );
	AddPlayerReport( ent, isMatchOver );
}

void StatsowFacade::OnClientJoinedTeam( edict_t *ent, int newTeam ) {
	ChatHandlersChain::Instance()->OnClientJoinedTeam( ent, newTeam );

	if( ent->r.client->team == TEAM_SPECTATOR ) {
		return;
	}
	if( newTeam != TEAM_SPECTATOR ) {
		return;
	}

	if ( GS_MatchState() != MATCH_STATE_PLAYTIME ) {
		return;
	}

	G_Printf( "Sending teamchange to MM, team %d to team %d\n", ent->r.client->team, newTeam );
	StatsowFacade::Instance()->AddPlayerReport( ent, false );
}

void StatsowFacade::AddPlayerReport( edict_t *ent, bool final ) {
	char uuid_buffer[UUID_BUFFER_SIZE];

	if( !ent->r.inuse ) {
		return;
	}

	// This code path should not be entered by race gametypes
	assert( !GS_RaceGametype() );

	if( !GS_MMCompatible() ) {
		return;
	}

	// Do not try to add player report if the match report has been discarded
	if( isDiscarded ) {
		return;
	}

	const auto *cl = ent->r.client;
	if( !cl || cl->team == TEAM_SPECTATOR ) {
		return;
	}

	constexpr const char *format = "StatsowFacade::AddPlayerReport(): %s" S_COLOR_WHITE " (%s)\n";
	G_Printf( format, cl->netname, cl->mm_session.ToString( uuid_buffer ) );

	ClientEntry *entry = FindEntryById( cl->mm_session );
	if( entry ) {
		AddToExistingEntry( ent, final, entry );
	} else {
		entry = NewPlayerEntry( ent, final );
		// put it to the list
		entry->next = clientEntriesHead;
		clientEntriesHead = entry;
	}

	ChatHandlersChain::Instance()->AddToReportStats( ent, &entry->respectStats );
}

void StatsowFacade::AddToExistingEntry( edict_t *ent, bool final, ClientEntry *e ) {
	auto *const cl = ent->r.client;

	// we can merge
	Q_strncpyz( e->netname, cl->netname, sizeof( e->netname ) );
	e->team = cl->team;
	e->timePlayed += ( level.time - cl->teamstate.timeStamp ) / 1000;
	e->final = final;

	e->stats.awards += cl->level.stats.awards;
	e->stats.score += cl->level.stats.score;

	for( const auto &keyAndValue : cl->level.stats ) {
		e->stats.AddToEntry( keyAndValue );
	}

	for( int i = 0; i < ( AMMO_TOTAL - AMMO_GUNBLADE ); i++ ) {
		auto &stats = e->stats;
		const auto &thatStats = cl->level.stats;
		stats.accuracy_damage[i] += thatStats.accuracy_damage[i];
		stats.accuracy_frags[i] += thatStats.accuracy_frags[i];
		stats.accuracy_hits[i] += thatStats.accuracy_hits[i];
		stats.accuracy_hits_air[i] += thatStats.accuracy_hits_air[i];
		stats.accuracy_hits_direct[i] += thatStats.accuracy_hits_direct[i];
		stats.accuracy_shots[i] += thatStats.accuracy_shots[i];
	}

	// merge awards
	// requires handling of duplicates
	MergeAwards( e->stats.awardsSequence, std::move( cl->level.stats.awardsSequence ) );

	// merge logged frags
	e->stats.fragsSequence.MergeWith( std::move( cl->level.stats.fragsSequence ) );
}

void StatsowFacade::MergeAwards( StatsSequence<gameaward_t> &to, StatsSequence<gameaward_t> &&from ) {
	for( const gameaward_t &mergable: from ) {
		// search for matching one
		auto it = to.begin();
		const auto end = to.end();
		for(; it != end; ++it ) {
			if( !strcmp( ( *it ).name, mergable.name ) ) {
				const_cast<gameaward_t &>( *it ).count += mergable.count;
				break;
			}
		}
		if( it != end ) {
			gameaward_t *added = to.New();
			added->name = mergable.name;
			added->count = mergable.count;
		}
	}

	// we can free the old awards
	from.Clear();
}

StatsowFacade::ClientEntry *StatsowFacade::FindEntryById( const mm_uuid_t &playerSessionId ) {
	for( ClientEntry *entry = clientEntriesHead; entry; entry = entry->next ) {
		if( entry->mm_session == playerSessionId ) {
			return entry;
		}
	}
	return nullptr;
}

StatsowFacade::RespectStats *StatsowFacade::FindRespectStatsById( const mm_uuid_t &playerSessionId ) {
	if( ClientEntry *entry = FindEntryById( playerSessionId ) ) {
		return &entry->respectStats;
	}
	return nullptr;
}

StatsowFacade::ClientEntry *StatsowFacade::NewPlayerEntry( edict_t *ent, bool final ) {
	auto *cl = ent->r.client;

	auto *const e = new( G_Malloc( sizeof( ClientEntry ) ) )ClientEntry;

	// fill in the data
	Q_strncpyz( e->netname, cl->netname, sizeof( e->netname ) );
	e->team = cl->team;
	e->timePlayed = ( level.time - cl->teamstate.timeStamp ) / 1000;
	e->final = final;
	e->mm_session = cl->mm_session;
	e->stats = std::move( cl->level.stats );
	return e;
}

void StatsowFacade::AddMetaAward( const edict_t *ent, const char *awardMsg ) {
	if( GS_MatchState() != MATCH_STATE_PLAYTIME ) {
		return;
	}

	if( ChatHandlersChain::Instance()->SkipStatsForClient( ent ) ) {
		return;
	}

	AddAward( ent, awardMsg );
}

void StatsowFacade::AddAward( const edict_t *ent, const char *awardMsg ) {
	if( isDiscarded ) {
		return;
	}

	if( GS_MatchState() != MATCH_STATE_PLAYTIME && GS_MatchState() != MATCH_STATE_POSTMATCH ) {
		return;
	}

	if( ChatHandlersChain::Instance()->SkipStatsForClient( ent ) ) {
		return;
	}

	auto *const stats = &ent->r.client->level.stats;
	// first check if we already have this one on the clients list
	gameaward_t *ga = nullptr;

	auto it = stats->awardsSequence.cbegin();
	const auto end = stats->awardsSequence.end();
	for(; it != end; ++it ) {
		if( !strcmp( ( *it ).name, awardMsg ) ) {
			ga = const_cast<gameaward_t *>( &( *it ) );
			break;
		}
	}

	if( it == end ) {
		ga = stats->awardsSequence.New();
		ga->name = G_RegisterLevelString( awardMsg );
		ga->count = 0;
	}

	ga->count++;
}

void StatsowFacade::AddFrag( const edict_t *attacker, const edict_t *victim, int mod ) {
	if( isDiscarded ) {
		return;
	}

	if( GS_MatchState() != MATCH_STATE_PLAYTIME ) {
		return;
	}

	// ch : frag log
	auto *const stats = &attacker->r.client->level.stats;
	loggedFrag_t *const lfrag = stats->fragsSequence.New();
	// TODO: Are these ID's required to be valid? What if there are monsters (not bots)?
	lfrag->attacker = attacker->r.client->mm_session;
	lfrag->victim = victim->r.client->mm_session;

	// Currently frags made using weak and strong kinds of ammo
	// share the same weapon index (thats what the stats server expect).
	// Thus, for MOD's of weak ammo, write the corresponding strong ammo value.
	static_assert( AMMO_GUNBLADE < AMMO_WEAK_GUNBLADE, "" );
	int weaponIndex = G_ModToAmmo( mod );
	if( weaponIndex >= AMMO_WEAK_GUNBLADE ) {
		// Eliminate weak ammo values shift
		weaponIndex -= AMMO_WEAK_GUNBLADE - AMMO_GUNBLADE;
	}
	// Shift weapon index so the first valid index correspond to Gunblade
	// (no-weapon kills will have a negative weapon index, and the stats server is aware of it).
	weaponIndex -= AMMO_GUNBLADE;
	lfrag->weapon = weaponIndex;
	// Changed to millis for the new stats server
	lfrag->time = game.serverTime - GS_MatchStartTime();
}

void StatsowFacade::WriteHeaderFields( QueryWriter &writer, int teamGame ) {
	// Note: booleans are transmitted as integers due to underlying api limitations
	writer << "match_id"       << trap_GetConfigString( CS_MATCHUUID );
	writer << "gametype"       << gs.gametypeName;
	writer << "map_name"       << level.mapname;
	writer << "server_name"    << trap_Cvar_String( "sv_hostname" );
	writer << "time_played"    << level.finalMatchDuration / 1000;
	writer << "time_limit"     << GS_MatchDuration() / 1000;
	writer << "score_limit"    << g_scorelimit->integer;
	writer << "is_instagib"    << ( GS_Instagib() ? 1 : 0 );
	writer << "is_team_game"   << ( teamGame ? 1 : 0 );
	writer << "is_race_game"   << ( GS_RaceGametype() ? 1 : 0 );
	writer << "mod_name"       << trap_Cvar_String( "fs_game" );
	writer << "utc_start_time" << game.utcMatchStartTime;

	if( g_autorecord->integer ) {
		writer << "demo_filename" << va( "%s%s", level.autorecord_name, game.demoExtension );
	}
}

void StatsowFacade::SendRegularReport() {
	int i, teamGame, duelGame;
	static const char *weapnames[WEAP_TOTAL] = { nullptr };

	// Feature: do not report matches with duration less than 1 minute (actually 66 seconds)
	if( level.finalMatchDuration <= SIGNIFICANT_MATCH_DURATION ) {
		return;
	}

	if( activeQuery ) {
		hasPendingResults = true;
		return;
	}

	QueryObject *query = activeQuery = trap_MM_NewPostQuery( "server/matchReport" );
	QueryWriter writer( query );

	// ch : race properties through GS_RaceGametype()

	// official duel frag support
	duelGame = GS_TeamBasedGametype() && GS_MaxPlayersInTeam() == 1 ? 1 : 0;

	// ch : fixme do mark duels as teamgames
	if( duelGame ) {
		teamGame = 0;
	} else if( !GS_TeamBasedGametype()) {
		teamGame = 0;
	} else {
		teamGame = 1;
	}

	WriteHeaderFields( writer, teamGame );

	// Write team properties (if any)
	if( teamlist[TEAM_ALPHA].numplayers > 0 && teamGame != 0 ) {
		writer << "teams" << '[';
		{
			for( i = TEAM_ALPHA; i <= TEAM_BETA; i++ ) {
				writer << '{';
				{
					writer << "name" << trap_GetConfigString( CS_TEAM_SPECTATOR_NAME + ( i - TEAM_SPECTATOR ));
					// TODO:... What do Statsow controllers expect?
					writer << "index" << i - TEAM_ALPHA;
					writer << "score" << teamlist[i].stats.score;
				}
				writer << '}';
			}
		}
		writer << ']';
	}

	// Provide the weapon indices for the stats server
	// Note that redundant weapons (that were not used) are allowed to be present here
	writer << "weapon_indices" << '{';
	{
		for( int j = 0; j < ( WEAP_TOTAL - WEAP_GUNBLADE ); ++j ) {
			weapnames[j] = GS_FindItemByTag( WEAP_GUNBLADE + j )->shortname;
			writer << weapnames[j] << j;
		}
	}
	writer << '}';

	// Write player properties
	writer << "players" << '[';
	for( ClientEntry *cl = clientEntriesHead; cl; cl = cl->next ) {
		writer << '{';
		cl->WriteToReport( writer, teamGame != 0, weapnames );
		writer << '}';
	}
	writer << ']';

	// TODO: We have to wait for completion then try again if needed and always delete the query anyway
	trap_MM_SendQuery( query );
}

void StatsowFacade::ClientEntry::WriteToReport( QueryWriter &writer, bool teamGame, const char **weaponNames ) {
	writer << "session_id"  << mm_session;
	writer << "name"        << netname;
	writer << "score"       << stats.score;
	writer << "time_played" << timePlayed;
	writer << "is_final"    << ( final ? 1 : 0 );
	if( teamGame != 0 ) {
		writer << "team" << team - TEAM_ALPHA;
	}

	writer << "respect_stats" << '{';
	{
		writer << "status";
		if( respectStats.hasViolatedCodex ) {
			writer << "violated";
		} else if( respectStats.hasIgnoredCodex ) {
			writer << "ignored";
		} else {
			writer << "followed";
			writer << "token_stats" << '{';
			{
				for( const auto &keyAndValue: respectStats ) {
					writer << keyAndValue.first << keyAndValue.second;
				}
			}
			writer << '}';
		}
	}

	if( respectStats.hasViolatedCodex || respectStats.hasIgnoredCodex ) {
		return;
	}

	writer << "various_stats" << '{';
	{
		for( const auto &keyAndValue: stats ) {
			writer << keyAndValue.first << keyAndValue.second;
		}
	}
	writer << '}';

	AddAwards( writer );
	AddWeapons( writer, weaponNames );
	AddFrags( writer );
}

void StatsowFacade::ClientEntry::AddAwards( QueryWriter &writer ) {
	writer << "awards" << '[';
	{
		for( const gameaward_t &award: stats.awardsSequence ) {
			writer << '{';
			{
				writer << "name"  << award.name;
				writer << "count" << award.count;
			}
			writer << '}';
		}
	}
	writer << ']';
}

void StatsowFacade::ClientEntry::AddFrags( QueryWriter &writer ) {
	writer << "log_frags" << '[';
	{
		for( const loggedFrag_t &frag: stats.fragsSequence ) {
			writer << '{';
			{
				writer << "victim" << frag.victim;
				writer << "weapon" << frag.weapon;
				writer << "time" << frag.time;
			}
			writer << '}';
		}
	}
	writer << ']';
}

// TODO: Should be lifted to gameshared
static inline double ComputeAccuracy( int hits, int shots ) {
	if( !hits ) {
		return 0.0;
	}
	if( hits == shots ) {
		return 100.0;
	}

	// copied from cg_scoreboard.c, but changed the last -1 to 0 (no hits is zero acc, right??)
	return ( std::min( (int)( std::floor( ( 100.0f * ( hits ) ) / ( (float)( shots ) ) + 0.5f ) ), 99 ) );
}

void StatsowFacade::ClientEntry::AddWeapons( QueryWriter &writer, const char **weaponNames ) {
	int i;

	// first pass calculate the number of weapons, see if we even need this section
	for( i = 0; i < ( AMMO_TOTAL - WEAP_TOTAL ); i++ ) {
		if( stats.accuracy_shots[i] > 0 ) {
			break;
		}
	}

	if( i >= ( AMMO_TOTAL - WEAP_TOTAL ) ) {
		return;
	}

	writer << "weapons" << '[';
	{
		int j;

		// we only loop thru the lower section of weapons since we put both
		// weak and strong shots inside the same weapon
		for( j = 0; j < AMMO_WEAK_GUNBLADE - WEAP_TOTAL; j++ ) {
			const int weak = j + ( AMMO_WEAK_GUNBLADE - WEAP_TOTAL );
			// Don't submit unused weapons
			if( stats.accuracy_shots[j] == 0 && stats.accuracy_shots[weak] == 0 ) {
				continue;
			}

			writer << '{';
			{
				writer << "name" << weaponNames[j];

				writer << "various_stats" << '{';
				{
					// STRONG
					int hits = stats.accuracy_hits[j];
					int shots = stats.accuracy_shots[j];

					writer << "strong_hits"   << hits;
					writer << "strong_shots"  << shots;
					writer << "strong_acc"    << ComputeAccuracy( hits, shots );
					writer << "strong_dmg"    << stats.accuracy_damage[j];
					writer << "strong_frags"  << stats.accuracy_frags[j];

					// WEAK
					hits = stats.accuracy_hits[weak];
					shots = stats.accuracy_shots[weak];

					writer << "weak_hits"   << hits;
					writer << "weak_shots"  << shots;
					writer << "weak_acc"    << ComputeAccuracy( hits, shots );
					writer << "weak_dmg"    << stats.accuracy_damage[weak];
					writer << "weak_frags"  << stats.accuracy_frags[weak];
				}
				writer << '}';
			}
			writer << '}';
		}
	}
	writer << ']';
}

void StatsowFacade::SendReport() {
	// TODO: check if MM is enabled

	if( GS_RaceGametype() ) {
		SendRaceReport();
		return;
	}

	if( !GS_MMCompatible() ) {
		ClearEntries();
		return;
	}

	// merge game.clients with game.quits
	for( edict_t *ent = game.edicts + 1; PLAYERNUM( ent ) < gs.maxclients; ent++ ) {
		AddPlayerReport( ent, true );
	}

	if( isDiscarded ) {
		G_Printf( S_COLOR_YELLOW "SendReport(): The match report has been discarded\n" );
	} else {
		// check if we have enough players to report (at least 2)
		if( clientEntriesHead && clientEntriesHead->next ) {
			SendRegularReport();
		}
	}

	ClearEntries();
}

void StatsowFacade::SendRaceReport() {
	if( !GS_RaceGametype() ) {
		G_Printf( S_COLOR_YELLOW "G_Match_RaceReport.. not race gametype\n" );
		return;
	}

	if( raceRuns.empty() ) {
		return;
	}

	if( activeQuery ) {
		hasPendingResults = true;
		return;
	}

	QueryObject *query = activeQuery = trap_MM_NewPostQuery( "server/matchReport" );

	QueryWriter writer( query );

	WriteHeaderFields( writer, false );

	writer << "race_runs" << '[';
	{
		for( const auto &rr: raceRuns ) {
			writer << '{';
			{
				// Setting session id and nickname is mutually exclusive
				if( rr.owner.IsValidSessionId() ) {
					writer << "session_id" << rr.owner;
				} else {
					writer << "nickname" << rr.nickname;
				}

				writer << "timestamp" << rr.utcTimestamp;

				writer << "times" << '[';
				{
					// Accessing the "+1" element is legal (its the final time). Supply it along with other times.
					for( int j = 0; j < rr.numSectors + 1; j++ )
						writer << rr.times[j];
				}
				writer << ']';
			}
			writer << '}';
		}
	}
	writer << ']';

	raceRunsToSend.Clear();
	raceRunsToSend.MergeWith( std::move( raceRuns ) );
	assert( raceRuns.empty() );
	assert( !raceRuns.empty() );

	// Make sure we've marked the query as an active
	assert( activeQuery );
	// Start sending again. Start polling activeQuery status next frame
	trap_MM_SendQuery( query );
}

#ifndef GAME_HARD_LINKED
// While most of the stuff is defined inline in the class
// and some parts that rely on qcommon too much are imported
// this implementation is specific for a binary we use this stuff in.
void QueryObject::FailWith( const char *format, ... ) {
	char buffer[2048];

	va_list va;
	va_start( va, format );
	Q_vsnprintfz( buffer, sizeof( buffer ), format, va );
	va_end( va );

	trap_Error( buffer );
}
#endif

RespectHandler::RespectHandler() {
	for( int i = 0; i < MAX_CLIENTS; ++i ) {
		entries[i].ent = game.edicts + i + 1;
	}
	Reset();
}

void RespectHandler::Reset() {
	for( ClientEntry &e: entries ) {
		e.Reset();
	}

	matchStartedAt = -1;
	lastFrameMatchState = MATCH_STATE_NONE;
}

void RespectHandler::Frame() {
	const auto matchState = GS_MatchState();
	// This is not 100% correct but is sufficient for message checks
	if( matchState == MATCH_STATE_PLAYTIME ) {
		if( lastFrameMatchState != MATCH_STATE_PLAYTIME ) {
			matchStartedAt = level.time;
		}
	}

	if( !GS_RaceGametype() ) {
		for( int i = 0; i < gs.maxclients; ++i ) {
			entries[i].CheckBehaviour( matchStartedAt );
		}
	}

	lastFrameMatchState = matchState;
}

bool RespectHandler::HandleMessage( const edict_t *ent, const char *message ) {
	// Race is another world...
	if( GS_RaceGametype() ) {
		return false;
	}

	// Allow public chatting in timeouts
	if( GS_MatchPaused() ) {
		return false;
	}

	const auto matchState = GS_MatchState();
	// Ignore until countdown
	if( matchState < MATCH_STATE_COUNTDOWN ) {
		return false;
	}

	return entries[ENTNUM( ent ) - 1].HandleMessage( message );
}

void RespectHandler::ClientEntry::Reset() {
	warnedAt = 0;
	firstJoinedTeamAt = 0;
	std::fill_n( firstSaidAt, 0, NUM_TOKENS );
	std::fill_n( lastSaidAt, 0, NUM_TOKENS );
	std::fill_n( numSaidTokens, 0, NUM_TOKENS );
	saidBefore = false;
	saidAfter = false;
	hasTakenCountdownHint = false;
	hasTakenStartHint = false;
	hasTakenFinalHint = false;
	hasIgnoredCodex = false;
	hasViolatedCodex = false;
}

bool RespectHandler::ClientEntry::HandleMessage( const char *message ) {
	// If has already violated the Codex
	if( hasViolatedCodex ) {
		return false;
	}

	// Now check for RnS tokens...
	if( CheckForTokens( message ) ) {
		return false;
	}

	const char *nickName = ent->r.client->netname;
	const char *warning = S_COLOR_GREY "Less talk, let's play!";
	if( GS_MatchState() < MATCH_STATE_PLAYTIME ) {
		// Print a warning only to the player
		PrintToClientScreen( "%s", warning );
		return false;
	}

	// Never warned (at start of the level)
	if( !warnedAt ) {
		warnedAt = level.time;
		PrintToClientScreen( "%s", warning );
		// Let the message be printed by default facilities
		return false;
	}

	const int64_t millisSinceLastWarn = level.time - warnedAt;
	// Don't warn again for occasional flood
	if( millisSinceLastWarn < 1000 ) {
		// Swallow messages silently
		return true;
	}

	// Allow speaking occasionally once per 5 minutes
	if( millisSinceLastWarn > 5 * 60 * 1000 ) {
		warnedAt = level.time;
		PrintToClientScreen( "%s", warning );
		return false;
	}

	hasViolatedCodex = true;
	// Print the message first
	G_ChatMsg( nullptr, ent, false, "%s", message );
	// Then announce
	AnnounceMisconductBehaviour( "violated" );
	// Interrupt handing of the message
	return true;
}

void RespectHandler::ClientEntry::AnnounceMisconductBehaviour( const char *action ) {
	// Ignore bots.
	// We plan to add R&S bot behaviour but do not currently want to touch the game module
	if( ent->r.svflags & SVF_FAKECLIENT ) {
		return;
	}

	const char *subject = S_COLOR_WHITE "Respect and Sportsmanship Codex";

	const char *outcome;
	if( !StatsowFacade::Instance()->IsMatchReportDiscarded() ) {
		outcome = S_COLOR_RED "No awards, no rating gain";
	} else {
		outcome = S_COLOR_RED "No awards given";
	}

	constexpr const char *format = S_COLOR_RED "BOOM! " S_COLOR_WHITE "%s" S_COLOR_RED " has %s %s! %s!\n";
	G_PrintMsg( nullptr, format, ent->r.client->netname, action, subject, outcome );

	PrintToClientScreen( S_COLOR_RED "You have %s R&S Codex...", action );
}

void RespectHandler::ClientEntry::PrintToClientScreen( const char *format, ... ) {
	char formatBuffer[MAX_STRING_CHARS];
	char commandBuffer[MAX_STRING_CHARS];

	va_list va;
	va_start( va, format );
	Q_vsnprintfz( formatBuffer, sizeof( formatBuffer ), format, va );
	va_end( va );

	// Make this message appear as an award at client-side
	Q_snprintfz( commandBuffer, sizeof( commandBuffer ), "aw \"%s\"", formatBuffer );
	trap_GameCmd( ent, commandBuffer );
}

// We still can't use C++17, here's a hack
class string_view {
	const char *s;
	const size_t len;
public:
	string_view( const char *s_ ) noexcept : s( s_ ), len( strlen( s ) ) {}
	const char *data() const { return s; }
	size_t size() const { return len; }
};

class RespectTokensRegistry {
	static const std::array<const string_view *, 10> ALIASES;

	static_assert( RespectHandler::NUM_TOKENS == 10, "" );
public:
	// For players staying in game during the match
	static constexpr auto SAY_AT_START_TOKEN_NUM = 2;
	static constexpr auto SAY_AT_END_TOKEN_NUM = 3;
	// For players joining or quitting mid-game
	static constexpr auto SAY_AT_JOINING_TOKEN_NUM = 0;
	static constexpr auto SAY_AT_QUITTING_TOKEN_NUM = 1;

	/**
	 * Finds a number of a token (a number of a token aliases group) the supplied string matches.
	 * @param p a pointer to a string data. Should not point to a white-space. A successful match advances this token.
	 * @return a number of token (of a token aliases group), a negative value on failure.
	 */
	static int MatchByToken( const char **p );

	static const char *TokenForNum( int num ) {
		assert( (unsigned )num < ALIASES.size() );
		return ALIASES[num][0].data();
	}
};

// Every chain must end with an empty string that acts as a terminator.
// Otherwise a runtime crash due to wrong loop upper bounds is expected.

static const string_view hiAliases[] = { "hi", "hello", "" };
static const string_view byeAliases[] = { "bb", "bye", "" };
static const string_view glhfAliases[] = { "glhf", "gl", "hf", "" };
static const string_view ggAliases[] = { "ggs", "gg", "bgs", "bg", "" };
static const string_view plzAliases[] = { "plz", "please", "" };
static const string_view tksAliases[] = { "tks", "thanks", "" };
static const string_view sozAliases[] = { "soz", "sorry", "" };
static const string_view smiley1Aliases[] = { ":)", "n1", "" };
static const string_view smiley2Aliases[] = { ":(", "np", "" };
static const string_view lolAliases[] = { "lol", "" };

const std::array<const string_view *, 10> RespectTokensRegistry::ALIASES = {
	hiAliases, byeAliases, glhfAliases, ggAliases, plzAliases,
	tksAliases, sozAliases, smiley1Aliases, smiley2Aliases, lolAliases
};

int RespectTokensRegistry::MatchByToken( const char **p ) {
	int tokenNum = 0;
	for( const string_view *tokenAliases: ALIASES ) {
		for( const string_view *alias = tokenAliases; alias->size(); alias++ ) {
			if( !Q_strnicmp( alias->data(), *p, alias->size() ) ) {
				*p += alias->size();
				return tokenNum;
			}
		}
		tokenNum++;
	}
	return -1;
}

bool RespectHandler::ClientEntry::CheckForTokens( const char *message ) {
	// Do not modify tokens count immediately
	// Either this routine fails completely or stats for all tokens get updated
	int numFoundTokens[NUM_TOKENS];
	std::fill_n( numFoundTokens, 0, NUM_TOKENS );

	const int64_t levelTime = level.time;

	const char *p = message;
	for(;; ) {
		while( ::isspace( *p ) ) {
			p++;
		}
		if( !*p ) {
			break;
		}
		int tokenNum = RespectTokensRegistry::MatchByToken( &p );
		if( tokenNum < 0 ) {
			return false;
		}
		numFoundTokens[tokenNum]++;
	}

	for( int tokenNum = 0; tokenNum < NUM_TOKENS; ++tokenNum ) {
		int numTokens = numFoundTokens[tokenNum];
		if( !numTokens ) {
			continue;
		}
		this->numSaidTokens[tokenNum] += numTokens;
		this->lastSaidAt[tokenNum] = levelTime;
	}

	return true;
}

void RespectHandler::ClientEntry::CheckBehaviour( const int64_t matchStartTime ) {
	if( !ent->r.inuse ) {
		return;
	}

	if( !ent->r.client->level.stats.had_playtime ) {
		return;
	}

	if( saidBefore && saidAfter ) {
		return;
	}

	const auto levelTime = level.time;
	const auto matchState = GS_MatchState();

	if( matchState == MATCH_STATE_COUNTDOWN ) {
		// If has just said "glhf"
		const int tokenNum = RespectTokensRegistry::SAY_AT_START_TOKEN_NUM;
		if( levelTime - lastSaidAt[tokenNum] < 64 ) {
			saidBefore = true;
		}
		if( !hasTakenCountdownHint ) {
			PrintToClientScreen( S_COLOR_CYAN "Say `%s` please!", RespectTokensRegistry::TokenForNum( tokenNum ) );
			hasTakenCountdownHint = true;
		}
		return;
	}

	if( matchState == MATCH_STATE_PLAYTIME ) {
		if( saidBefore ) {
			return;
		}

		int tokenNum;
		int64_t countdownStartTime;
		// Say "glhf" being in-game from the beginning or "hi" when joining
		if( firstJoinedTeamAt <= matchStartTime ) {
			countdownStartTime = matchStartTime;
			tokenNum = RespectTokensRegistry::SAY_AT_START_TOKEN_NUM;
		} else {
			countdownStartTime = firstJoinedTeamAt;
			tokenNum = RespectTokensRegistry::SAY_AT_JOINING_TOKEN_NUM;
		}

		if( levelTime - lastSaidAt[tokenNum] < 64 ) {
			saidBefore = true;
			return;
		}

		if( levelTime - countdownStartTime < 1500 ) {
			return;
		}

		if( !hasTakenStartHint ) {
			PrintToClientScreen( S_COLOR_CYAN "Say `%s` please!", RespectTokensRegistry::TokenForNum( tokenNum ) );
			hasTakenStartHint = true;
			return;
		}

		if( !hasIgnoredCodex && levelTime - countdownStartTime > 10000 ) {
			// The misconduct behaviour is going to be detected inevitably.
			// This is just to prevent massive console spam at the same time.
			if( random() > 0.95f ) {
				hasIgnoredCodex = true;
				AnnounceMisconductBehaviour( "ignored" );
			}
			return;
		}
	}

	if( matchState != MATCH_STATE_POSTMATCH ) {
		return;
	}

	if( levelTime - lastSaidAt[RespectTokensRegistry::SAY_AT_END_TOKEN_NUM] < 64 ) {
		saidAfter = true;
		if( saidBefore && !hasViolatedCodex ) {
			G_PlayerAward( ent, S_COLOR_CYAN "Fair play!" );
			G_PrintMsg( ent, "Your stats and awards have been confirmed!\n" );
		}
	}

	if( !saidAfter && saidBefore && !hasViolatedCodex ) {
		// TODO: Say this hint 1 second after the match
		if( !hasTakenFinalHint ) {
			// Say "gg" at the end regardless of being in-game from the beginning or joining mid-game
			PrintToClientScreen( S_COLOR_CYAN "Say `gg` please!" );
			hasTakenFinalHint = true;
		}
		return;
	}

	if( !hasTakenFinalHint ) {
		if( hasIgnoredCodex || hasViolatedCodex ) {
			PrintToClientScreen( "Be nice next time please..." );
		}
		hasTakenFinalHint = true;
	}
}

void RespectHandler::ClientEntry::OnClientDisconnected() {
	if( GS_MatchState() != MATCH_STATE_PLAYTIME ) {
		return;
	}

	if( !ent->r.client->level.stats.had_playtime ) {
		return;
	}

	// Skip bots currently
	if( ent->r.svflags & SVF_FAKECLIENT ) {
		return;
	}

	if( !saidBefore || hasViolatedCodex ) {
		return;
	}

	constexpr int sayAtQuitting = RespectTokensRegistry::SAY_AT_QUITTING_TOKEN_NUM;
	constexpr int sayAtEnd = RespectTokensRegistry::SAY_AT_END_TOKEN_NUM;

	int64_t lastByeTokenTime = -1;
	if( lastSaidAt[sayAtQuitting] > lastByeTokenTime ) {
		lastByeTokenTime = lastSaidAt[sayAtQuitting];
	} else if( lastSaidAt[sayAtQuitting] > lastByeTokenTime ) {
		lastByeTokenTime = lastSaidAt[sayAtQuitting];
	}

	// Check whether its substantially overridden by other tokens
	for( int i = 0; i < NUM_TOKENS; ++i ) {
		if( i == sayAtEnd || i == sayAtQuitting ) {
			continue;
		}
		if( lastSaidAt[i] > lastByeTokenTime + 3000 ) {
			lastByeTokenTime = -1;
			break;
		}
	}

	if( warnedAt < lastByeTokenTime ) {
		saidAfter = true;
		return;
	}

	assert( !saidAfter );
	const char *outcome = "";
	if( !StatsowFacade::Instance()->IsMatchReportDiscarded() ) {
		outcome = " No rating progress, no awards saved!";
	}

	const char *format = "%s" S_COLOR_YELLOW " chickened and left the game.%s\n";
	G_Printf( format, ent->r.client->netname, outcome );
}

void RespectHandler::ClientEntry::OnClientJoinedTeam( int newTeam ) {
	if( newTeam == TEAM_SPECTATOR ) {
		return;
	}

	if( GS_MatchState() > MATCH_STATE_PLAYTIME ) {
		return;
	}

	if( !firstJoinedTeamAt ) {
		firstJoinedTeamAt = level.time;
	}

	// Check whether there is already Codex violation recorded for the player during this match
	mm_uuid_t clientSessionId = this->ent->r.client->mm_session;
	if( !clientSessionId.IsValidSessionId() ) {
		return;
	}

	auto *respectStats = StatsowFacade::Instance()->FindRespectStatsById( clientSessionId );
	if( !respectStats ) {
		return;
	}

	this->hasViolatedCodex = respectStats->hasViolatedCodex;
	this->hasIgnoredCodex = respectStats->hasIgnoredCodex;
}

void RespectHandler::ClientEntry::AddToReportStats( StatsowFacade::RespectStats *reportedStats ) {
	if( reportedStats->hasViolatedCodex ) {
		return;
	}

	if( hasViolatedCodex ) {
		reportedStats->Clear();
		reportedStats->hasViolatedCodex = true;
		reportedStats->hasIgnoredCodex = hasIgnoredCodex;
		return;
	}

	if( hasIgnoredCodex ) {
		reportedStats->Clear();
		reportedStats->hasIgnoredCodex = true;
		return;
	}

	if( reportedStats->hasIgnoredCodex ) {
		return;
	}

	for( int i = 0; i < NUM_TOKENS; ++i ) {
		if( !numSaidTokens[i] ) {
			continue;
		}
		const char *token = RespectTokensRegistry::TokenForNum( i );
		reportedStats->AddToEntry( token, numSaidTokens[i] );
	}
}