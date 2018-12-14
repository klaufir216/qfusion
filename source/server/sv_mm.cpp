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

#include <time.h>       // just for dev

#include "server.h"
#include "../gameshared/q_shared.h"

#include "../matchmaker/mm_common.h"
#include "../matchmaker/mm_rating.h"
#include "../matchmaker/mm_query.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <functional>

// interval between successive attempts to get match UUID from the mm
#define SV_MM_MATCH_UUID_FETCH_INTERVAL     20  // in seconds

/*
* private vars
*/
static bool sv_mm_initialized = false;
static mm_uuid_t sv_mm_session;

// local session counter
static int64_t sv_mm_last_heartbeat;
static bool sv_mm_logout_semaphore = false;

// flag for gamestate = game-on
static bool sv_mm_gameon = false;

static char sv_mm_match_uuid[37];
static int64_t sv_mm_next_match_uuid_fetch;
static void (*sv_mm_match_uuid_callback_fn)( const char *uuid );

/*
* public vars
*/
cvar_t *sv_mm_authkey;
cvar_t *sv_mm_enable;
cvar_t *sv_mm_loginonly;

/*
* prototypes
*/
static bool SV_MM_Login( void );
static void SV_MM_Logout( bool force );
static void SV_MM_GetMatchUUIDThink( void );

/*
* Utilities
*/
static client_t *SV_MM_ClientForSession( mm_uuid_t session_id ) {
	int i;
	client_t *cl;

	for( i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++ ) {
		// also ignore zombies?
		if( cl->state == CS_FREE ) {
			continue;
		}

		if( Uuid_Compare( cl->mm_session, session_id ) ) {
			return cl;
		}
	}

	return NULL;
}

class QueryObject *SV_MM_NewGetQuery( const char *url ) {
	return QueryObject::NewGetQuery( url, sv_ip->string );
}

class QueryObject *SV_MM_NewPostQuery( const char *url ) {
	return QueryObject::NewPostQuery( url, sv_ip->string );
}

void SV_MM_DeleteQuery( class QueryObject *query ) {
	QueryObject::DeleteQuery( query );
}

bool SV_MM_SendQuery( class QueryObject *query ) {
	// TODO: Check?
	query->SetServerSession( sv_mm_session );
	return query->SendForStatusPolling();
}

// TODO: instead of this, factor ClientDisconnect to game module which can flag
// the gamestate in that function
void SV_MM_GameState( bool gameon ) {
	sv_mm_gameon = gameon;
}

void SV_MM_Heartbeat() {
	if( !sv_mm_initialized || !Uuid_IsValidSessionId( sv_mm_session ) ) {
		return;
	}

	QueryObject *query = QueryObject::NewPostQuery( "server/heartbeat", sv_ip->string );
	if( !query ) {
		return;
	}

	query->SetServerSession( sv_mm_session );
	query->SendDeletingOnCompletion([]( QueryObject * ) {});
}

void SV_MM_ClientDisconnect( client_t *client ) {
	if( !sv_mm_initialized || !Uuid_IsValidSessionId( sv_mm_session ) ) {
		return;
	}

	// do we need to tell about anonymous clients?
	if( !Uuid_IsValidSessionId( client->mm_session ) ) {
		return;
	}

	QueryObject *query = QueryObject::NewPostQuery( "server/clientDisconnect", sv_ip->string );
	if( !query ) {
		return;
	}

	query->SetServerSession( sv_mm_session );
	query->SetClientSession( client->mm_session );
	query->SendDeletingOnCompletion( [=]( QueryObject *query ) {
		if( query->HasSucceeded() ) {
			char buffer[UUID_BUFFER_SIZE];
			Com_Printf( "SV_MM_ClientDisconnect: Acknowledged %s\n", client->mm_session.ToString( buffer ) );
		} else {
			Com_Printf( "SV_MM_ClientDisconnect: Error\n" );
		}
	});
}

struct ScopeGuard {
	const std::function<void()> &atExit;
	bool suppressed { false };

	explicit ScopeGuard( const std::function<void()> & atExit_ ) : atExit( atExit_ ) {}

	~ScopeGuard() {
		if( !suppressed ) {
			atExit();
		}
	}

	void Suppress() { suppressed = true; }
};

static void SV_MM_ClientConnectDone( QueryObject *query, client_t *cl ) {
	bool userinfo_changed;
	char uuid_buffer[UUID_BUFFER_SIZE];

	// Happens if a game module rejects connection
	if( !cl->edict ) {
		Com_Printf( "SV_MM_ClientConnect: The client is no longer valid\n" );
		return;
	}

	auto onAnyOutcome = [&]() {
		if( userinfo_changed ) {
			SV_UserinfoChanged( cl );
		}

		const char *format = "SV_MM_ClientConnect: %s with session id %s\n";
		Com_Printf( format, cl->name, Uuid_ToString( uuid_buffer, cl->mm_session ) );
	};

	ScopeGuard scopeGuard( onAnyOutcome );

	auto onFailure = [&]() {
		// unable to validate client, either kick him out or force local session
		if( sv_mm_loginonly->integer ) {
			SV_DropClient( cl, DROP_TYPE_GENERAL, "%s", "Error: This server requires login. Create account at " APP_URL );
			return;
		}

		// TODO: Does it have to be unique?
		mm_uuid_t session_id = Uuid_FFFsUuid();
		Uuid_ToString( uuid_buffer, session_id );
		Com_Printf( "SV_MM_ClientConnect: Forcing local_session %s on client %s\n", uuid_buffer, cl->name );
		cl->mm_session = session_id;
		userinfo_changed = true;
	};

	ScopeGuard failureGuard( onFailure );

	if( !query->HasSucceeded() ) {
		Com_Printf( "SV_MM_ClientConnect: Remote or network failure\n" );
		return;
	}

	if( !query->ResponseJsonRoot() ) {
		Com_Printf( "SV_MM_ClientConnect: failed to parse a raw response\n" );
		return;
	}

	if( query->GetRootDouble( "banned", 0 ) != 0 ) {
		const char *reason = query->GetRootString( "reason", "" );
		if( !*reason ) {
			reason = "Your account at " APP_URL " has been banned.";
		}

		SV_DropClient( cl, DROP_TYPE_GENERAL, "Error: %s", reason );
		return;
	}

	if( query->GetRootDouble( "status", 0 ) == 0 ) {
		const char *error = query->GetRootString( "error", "" );
		if( *error ) {
			Com_Printf( "SV_MM_ClientConnect: Request error at remote host: %s\n", error );
		} else {
			Com_Printf( "SV_MM_ClientConnect: Bad or missing response status\n" );
		}
		return;
	}

	// Note: we have omitted client session id comparisons,
	// it has to be performed on server anyway and a mismatch yields a failed response.

	ObjectReader rootReader( query->ResponseJsonRoot() );
	// TODO: This would have been better if we could rely on std::optional support and just return optional<ObjectReader>
	cJSON *infoSection = rootReader.GetObject( "player_info" );
	if( !infoSection ) {
		Com_Printf( "SV_MM_ParseResponse: Missing `player_info` section\n" );
		return;
	}

	ObjectReader infoReader( infoSection );
	const char *login = infoReader.GetString( "login", "" );
	if( !*login ) {
		Com_Printf( "SV_MM_ParseResponse: Missing `login` field\n" );
		return;
	}

	Q_strncpyz( cl->mm_login, login, sizeof( cl->mm_login ) );
	if( !Info_SetValueForKey( cl->userinfo, "cl_mm_login", login ) ) {
		// TODO: What to do in this case?
		Com_Printf( "Failed to set infokey 'cl_mm_login' for player %s\n", login );
	}

	const char *mmflags = query->GetRootString( "mmflags", "" );
	if( *mmflags ) {
		if( !Info_SetValueForKey( cl->userinfo, "mmflags", mmflags ) ) {
			Com_Printf( "Failed to set infokey 'mmflags' for player %s\n", login );
		}
	}

	userinfo_changed = true;

	// Don't call the code for the "failure" path at scope exit
	failureGuard.Suppress();

	// Again this cries for optionals
	cJSON *ratingsSection = rootReader.GetArray( "ratings" );
	if( !ratingsSection || !ge ) {
		return;
	}

	ArrayReader ratingsReader( ratingsSection );
	edict_t *const ent = EDICT_NUM( ( cl - svs.clients ) + 1 );
	while( !ratingsReader.IsDone() ) {
		// This cries for optionals too
		if( !ratingsReader.IsAtObject() ) {
			Com_Printf( "Warning: an entry in ratings array is not a JSON object\n" );
			break;
		}
		ObjectReader entryReader( ratingsReader.GetChildObject() );
		const char *gametype = entryReader.GetString( "gametype" );
		const double rating = entryReader.GetDouble( "rating" );
		const double deviation = entryReader.GetDouble( "deviation" );
		if( !gametype || std::isnan( rating ) || std::isnan( deviation ) ) {
			Com_Printf( "Warning: an entry in ratings array is malformed\n" );
			break;
		}

		ge->AddRating( ent, gametype, (float)rating, (float)deviation );
		ratingsReader.Next();
	}
}

mm_uuid_t SV_MM_ClientConnect( client_t *client, const netadr_t *address, char *, mm_uuid_t ticket, mm_uuid_t session_id ) {
	// return of -1 is not an error, it just marks a dummy local session
	if( !sv_mm_initialized || !Uuid_IsValidSessionId( sv_mm_session ) ) {
		return Uuid_FFFsUuid();
	}

	if( !Uuid_IsValidSessionId( session_id ) || !Uuid_IsValidSessionId( ticket ) ) {
		if( sv_mm_loginonly->integer ) {
			Com_Printf( "SV_MM_ClientConnect: Login-only\n");
			return Uuid_ZeroUuid();
		}
		Com_Printf( "SV_MM_ClientConnect: Invalid session id or ticket, marking as anonymous\n" );
		return Uuid_FFFsUuid();
	}

	// push a request
	QueryObject *query = QueryObject::NewPostQuery( "/server/clientConnect", sv_ip->string );
	if( !query ) {
		return Uuid_ZeroUuid();
	}

	query->SetServerSession( sv_mm_session );
	query->SetTicket( ticket );
	query->SetClientSession( session_id );
	query->SetClientAddress( NET_AddressToString( address ) );
	query->SendDeletingOnCompletion( [=]( QueryObject *query ) { SV_MM_ClientConnectDone( query, client ); } );

	return session_id;
}

void SV_MM_Frame() {
	int64_t time;

	if( sv_mm_enable->modified ) {
		if( sv_mm_enable->integer && !sv_mm_initialized ) {
			SV_MM_Login();
		} else if( !sv_mm_enable->integer && sv_mm_initialized ) {
			SV_MM_Logout( false );
		}

		sv_mm_enable->modified = false;
	}

	if( sv_mm_initialized ) {
		if( sv_mm_logout_semaphore ) {
			// logout process is finished so we can shutdown game
			SV_MM_Shutdown( false );
			sv_mm_logout_semaphore = false;
			return;
		}

		// heartbeat
		time = Sys_Milliseconds();
		if( ( sv_mm_last_heartbeat + MM_HEARTBEAT_INTERVAL ) < time ) {
			SV_MM_Heartbeat();
			sv_mm_last_heartbeat = time;
		}

		SV_MM_GetMatchUUIDThink();
	}
}

bool SV_MM_Initialized() {
	return sv_mm_initialized;
}


/*
* SV_MM_Logout
*/
static void SV_MM_Logout( bool force ) {
	if( !sv_mm_initialized || !Uuid_IsValidSessionId( sv_mm_session ) ) {
		return;
	}

	QueryObject *query = QueryObject::NewPostQuery( "server/logout", sv_ip->string );
	if( !query ) {
		return;
	}

	sv_mm_logout_semaphore = false;

	query->SetServerSession( sv_mm_session );
	query->SendDeletingOnCompletion( [=]( QueryObject *query ) {
		Com_Printf( "SV_MM_Logout: Loggin off..\n" );
		// ignore response-status and just mark us as logged-out
		sv_mm_logout_semaphore = true;
	});

	if( !force ) {
		return;
	}

	const auto startTime = Sys_Milliseconds();
	while( !sv_mm_logout_semaphore && Sys_Milliseconds() < ( startTime + MM_LOGOUT_TIMEOUT ) ) {
		QueryObject::Poll();

		Sys_Sleep( 10 );
	}

	if( !sv_mm_logout_semaphore ) {
		Com_Printf( "SV_MM_Logout: Failed to force logout\n" );
	} else {
		Com_Printf( "SV_MM_Logout: force logout successful\n" );
	}

	sv_mm_logout_semaphore = false;

	// dont call this, we are coming from shutdown
	// SV_MM_Shutdown( false );
}

static QueryObject *sv_login_query = nullptr;

static void SV_MM_LoginDone( QueryObject *query ) {
	char uuid_buffer[UUID_BUFFER_SIZE];

	sv_mm_initialized = false;
	sv_login_query = nullptr;

	if( !query->HasSucceeded() ) {
		Com_Printf( "SV_MM_Login_Done: Error\n" );
		Cvar_ForceSet( sv_mm_enable->name, "0" );
		return;
	}

	Com_DPrintf( "SV_MM_Login: %s\n", query->RawResponse() );

	ScopeGuard failureGuard([&]() {
		Com_Printf( "SV_MM_Login: Failed, no session id\n" );
		Cvar_ForceSet( sv_mm_enable->name, "0" );
	});

	if( !query->ResponseJsonRoot() ) {
		Com_Printf( "SV_MM_Login: Failed to parse data\n" );
		return;
	}

	const auto status = query->GetRootDouble( "status", 0.0 );
	if( status == 0 ) {
		const char *error = query->GetRootString( "error", "" );
		if( *error ) {
			Com_Printf( "SV_MM_Login: Request error at remote host: %s\n", error );
		} else {
			Com_Printf( "SV_MM_Login: Unspecified error at remote host\n" );
		}
		return;
	}

	const char *sessionString = query->GetRootString( "session_id", "" );
	if( !Uuid_FromString( sessionString, &sv_mm_session ) ) {
		Com_Printf( "SV_MM_Login: Failed to parse session string %s\n", sessionString );
		return;
	}

	sv_mm_initialized = Uuid_IsValidSessionId( sv_mm_session );
	if( !sv_mm_initialized ) {
		return;
	}

	Com_Printf( "SV_MM_Login: Success, session id %s\n", Uuid_ToString( uuid_buffer, sv_mm_session ) );
	failureGuard.Suppress();
}

/*
* SV_MM_Login
*/
static bool SV_MM_Login() {
	if( !sv_mm_initialized ) {
		return false;
	}

	if( sv_login_query ) {
		return false;
	}

	if( sv_mm_authkey->string[0] == '\0' ) {
		Cvar_ForceSet( sv_mm_enable->name, "0" );
		return false;
	}

	Com_Printf( "SV_MM_Login: Creating query\n" );

	QueryObject *query = QueryObject::NewPostQuery( "server/login", sv_ip->string );
	if( !query ) {
		return false;
	}

	query->SetAuthKey( sv_mm_authkey->string );
	query->SetPort( va( "%d", sv_port->integer ) );
	query->SetServerName( sv.configstrings[CS_HOSTNAME] );
	query->SetServerAddress( sv_ip->string );
	query->SetDemosBaseUrl( sv_uploads_demos_baseurl->string );

	sv_login_query = query;

	query->SendDeletingOnCompletion( [=]( QueryObject *q ) { SV_MM_LoginDone( q ); } );
	return true;
}

static QueryObject *sv_mm_match_uuid_fetch_query = nullptr;

static void SV_MM_MatchUuidDone( QueryObject *query ) {
	// set the repeat timer, which will be ignored in case we successfully parse the response
	sv_mm_next_match_uuid_fetch = Sys_Milliseconds() + SV_MM_MATCH_UUID_FETCH_INTERVAL * 1000;
	sv_mm_match_uuid_fetch_query = nullptr;

	if( !query->HasSucceeded() ) {
		return;
	}

	Com_DPrintf( "SV_MM_GetMatchUUID: %s\n", query->RawResponse() );

	const cJSON *root = query->ResponseJsonRoot();
	if( !root ) {
		Com_Printf( "SV_MM_GetMatchUUID: Failed to parse data\n" );
		return;
	}

	const char *uuidString = cJSON_GetObjectItem( const_cast<cJSON *>(root), "uuid" )->valuestring;
	Q_strncpyz( sv_mm_match_uuid, uuidString, UUID_BUFFER_SIZE );
	if( sv_mm_match_uuid_callback_fn ) {
		// fire the callback function
		sv_mm_match_uuid_callback_fn( sv_mm_match_uuid );
	}
}

/*
* SV_MM_GetMatchUUIDThink
*
* Repeatedly query the matchmaker for match UUID until we get one.
*/
static void SV_MM_GetMatchUUIDThink() {
	if( !sv_mm_initialized || !Uuid_IsValidSessionId( sv_mm_session ) ) {
		return;
	}

	if( sv_mm_next_match_uuid_fetch > Sys_Milliseconds() ) {
		// not ready yet
		return;
	}

	if( sv_mm_match_uuid_fetch_query ) {
		// already in progress
		return;
	}

	if( sv_mm_match_uuid[0] != '\0' ) {
		// we have already queried the server
		return;
	}

	// ok, get it now!
	Com_DPrintf( "SV_MM_GetMatchUUIDThink: Creating query\n" );

	QueryObject *query = QueryObject::NewPostQuery( "server/matchUuid", sv_ip->string );
	if( !query ) {
		return;
	}

	query->SetServerSession( sv_mm_session );
	sv_mm_match_uuid_fetch_query = query;
	query->SendDeletingOnCompletion( [=]( QueryObject *q ) { SV_MM_MatchUuidDone( q ); } );
}

/*
* SV_MM_GetMatchUUID
*
* Start querying the server for match UUID. Fire the callback function
* upon success.
*/
void SV_MM_GetMatchUUID( void ( *callback_fn )( const char *uuid ) ) {
	if( !sv_mm_initialized ) {
		return;
	}
	if( sv_mm_match_uuid_fetch_query ) {
		// already in progress
		return;
	}
	if( sv_mm_next_match_uuid_fetch > Sys_Milliseconds() ) {
		// not ready yet
		return;
	}

	sv_mm_match_uuid[0] = '\0';
	sv_mm_match_uuid_callback_fn = callback_fn;

	// think now!
	sv_mm_next_match_uuid_fetch = Sys_Milliseconds();
	SV_MM_GetMatchUUIDThink();
}

/*
* SV_MM_Init
*/
void SV_MM_Init() {
	sv_mm_initialized = false;
	sv_mm_session = Uuid_ZeroUuid();
	sv_mm_last_heartbeat = 0;
	sv_mm_logout_semaphore = false;

	sv_mm_gameon = false;

	sv_mm_match_uuid[0] = '\0';
	sv_mm_next_match_uuid_fetch = Sys_Milliseconds();
	sv_mm_match_uuid_fetch_query = nullptr;
	sv_mm_match_uuid_callback_fn = nullptr;

	/*
	* create cvars
	* ch : had to make sv_mm_enable to cmdline only, because of possible errors
	* if enabled while players on server
	*/
	sv_mm_enable = Cvar_Get( "sv_mm_enable", "0", CVAR_ARCHIVE | CVAR_NOSET | CVAR_SERVERINFO );
	sv_mm_loginonly = Cvar_Get( "sv_mm_loginonly", "0", CVAR_ARCHIVE | CVAR_SERVERINFO );

	// this is used by game, but to pass it to client, we'll initialize it in sv
	Cvar_Get( "sv_skillRating", va( "%.0f", MM_RATING_DEFAULT ), CVAR_READONLY | CVAR_SERVERINFO );

	// TODO: remove as cvar
	sv_mm_authkey = Cvar_Get( "sv_mm_authkey", "", CVAR_ARCHIVE );

	/*
	* login
	*/
	sv_login_query = nullptr;
	//if( sv_mm_enable->integer )
	//	SV_MM_Login();
	sv_mm_enable->modified = true;
}

void SV_MM_Shutdown( bool logout ) {
	if( !sv_mm_initialized ) {
		return;
	}

	Com_Printf( "SV_MM_Shutdown..\n" );

	if( logout ) {
		// logout is always force in here
		SV_MM_Logout( true );
	}

	Cvar_ForceSet( "sv_mm_enable", "0" );

	sv_mm_gameon = false;

	sv_mm_last_heartbeat = 0;
	sv_mm_logout_semaphore = false;

	sv_mm_initialized = false;
	sv_mm_session = Uuid_ZeroUuid();
}
