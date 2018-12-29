#include "snap_tables.h"
#include "../gameshared/gs_public.h"
#include "../gameshared/q_comref.h"
#include "../qalgo/SingletonHolder.h"

static SingletonHolder<SnapShadowTable> shadowTableHolder;

void SnapShadowTable::Init() {
	::shadowTableHolder.Init();
}

void SnapShadowTable::Shutdown() {
	::shadowTableHolder.Shutdown();
}

SnapShadowTable *SnapShadowTable::Instance() {
	return ::shadowTableHolder.Instance();
}

SnapShadowTable::SnapShadowTable() {
	// Get zeroed memory
	table = (bool *)::calloc( ( MAX_CLIENTS ) * ( MAX_EDICTS ) * sizeof( bool ), 1 );
	// Shouldn't happen?
	if( !table ) {
		Com_Error( ERR_FATAL, "Can't allocate snapshots entity shadow table" );
	}
}

static SingletonHolder<SnapVisTable> visTableHolder;

void SnapVisTable::Init( cmodel_state_t *cms ) {
	::visTableHolder.Init( cms );
}

void SnapVisTable::Shutdown() {
	::visTableHolder.Shutdown();
}

SnapVisTable *SnapVisTable::Instance() {
	return ::visTableHolder.Instance();
}

SnapVisTable::SnapVisTable( cmodel_state_t *cms_ ): cms( cms_ ) {
	table = (int8_t *)::calloc( ( MAX_CLIENTS ) * ( MAX_CLIENTS ) * sizeof( int8_t ), 1 );
	// Shouldn't happen?
	if( !table ) {
		Com_Error( ERR_FATAL, "Can't allocate snapshots visibility table" );
	}
}

bool SnapVisTable::CastRay( const vec3_t from, const vec3_t to ) {
	// Account for degenerate cases
	if( DistanceSquared( from, to ) < 16 * 16 ) {
		return true;
	}

	trace_t trace;
	CM_TransformedBoxTrace( cms, &trace, (float *)from, (float *)to, vec3_origin, vec3_origin, NULL, MASK_SOLID, NULL, NULL );

	if( trace.fraction == 1.0f ) {
		return true;
	}

	if( !( trace.contents & CONTENTS_TRANSLUCENT ) ) {
		return false;
	}

	vec3_t rayDir;
	VectorSubtract( to, from, rayDir );
	VectorNormalize( rayDir );

	// Do 8 attempts to continue tracing.
	// It might seem that the threshold could be lower,
	// but keep in mind situations like on wca1
	// while looking behind a glass tube being behind a glass wall itself.
	for( int i = 0; i < 8; ++i ) {
		vec3_t rayStart;
		VectorMA( trace.endpos, 2.0f, rayDir, rayStart );

		CM_TransformedBoxTrace( cms, &trace, rayStart, (float *)to, vec3_origin, vec3_origin, NULL, MASK_SOLID, NULL, NULL );

		if( trace.fraction == 1.0f ) {
			return true;
		}

		if( !( trace.contents & CONTENTS_TRANSLUCENT ) ) {
			return false;
		}
	}

	return false;
}

static inline void GetRandomPointInBox( const vec3_t origin, const vec3_t mins, const vec3_t size, vec3_t result ) {
	result[0] = origin[0] + mins[0] + random() * size[0];
	result[1] = origin[1] + mins[1] + random() * size[1];
	result[2] = origin[2] + mins[2] + random() * size[2];
}

bool SnapVisTable::DoCullingByCastingRays( const edict_t *clientEnt, const vec3_t viewOrigin, const edict_t *targetEnt ) {
	const gclient_t *const povClient = clientEnt->r.client;
	const gclient_t *const targetClient = targetEnt->r.client;

	// This method is internal and should be called supplying proper arguments
	assert( povClient && targetClient );

	// Do not use vis culling for fast moving clients/entities due to being prone to glitches

	const float *povVelocity = povClient->ps.pmove.velocity;
	float squarePovVelocity = VectorLengthSquared( povVelocity );
	if( squarePovVelocity > 1100 * 1100 ) {
		return false;
	}

	const float *targetVelocity = targetEnt->r.client->ps.pmove.velocity;
	float squareTargetVelocity = VectorLengthSquared( targetVelocity );
	if( squareTargetVelocity > 1100 * 1100 ) {
		return false;
	}

	if( squarePovVelocity > 800 * 800 && squareTargetVelocity > 800 * 800 ) {
		return false;
	}

	vec3_t to;
	VectorCopy( targetEnt->s.origin, to );

	// Do a first raycast to the entity origin for a fast cutoff
	if( CastRay( viewOrigin, to ) ) {
		return false;
	}

	// Do a second raycast at the entity chest/eyes level
	to[2] += targetClient->ps.viewheight;
	if( CastRay( viewOrigin, to ) ) {
		return false;
	}

	// Test a random point in entity bounds now
	GetRandomPointInBox( targetEnt->s.origin, targetEnt->r.mins, targetEnt->r.size, to );
	if( CastRay( viewOrigin, to ) ) {
		return false;
	}

	// Test all bbox corners at the current position.
	// Prevent missing a player that should be clearly visible.

	vec3_t bounds[2];
	// Shrink bounds by 2 units to avoid ending in a solid if the entity contacts some brushes.
	for( int i = 0; i < 3; ++i ) {
		bounds[0][i] = targetEnt->r.mins[i] + 2.0f;
		bounds[1][i] = targetEnt->r.maxs[i] - 2.0f;
	}

	for( int i = 0; i < 8; ++i ) {
		to[0] = targetEnt->s.origin[0] + bounds[(i >> 2) & 1][0];
		to[1] = targetEnt->s.origin[1] + bounds[(i >> 1) & 1][1];
		to[2] = targetEnt->s.origin[2] + bounds[(i >> 0) & 1][2];
		if( CastRay( viewOrigin, to ) ) {
			return false;
		}
	}

	// There is no need to extrapolate
	if( squarePovVelocity < 10 * 10 && squareTargetVelocity < 10 * 10 ) {
		return true;
	}

	// We might think about skipping culling for high-ping players but pings are easily mocked from a client side.
	// The game is barely playable for really high pings anyway.

	float xerpTimeSeconds = 0.001f * ( 100 + povClient->r.ping );
	clamp_high( xerpTimeSeconds, 0.275f );

	// We want to test the trajectory in "continuous" way.
	// Use a fixed small trajectory step and adjust the timestep using it.
	float timeStepSeconds;
	if( squareTargetVelocity > squarePovVelocity ) {
		timeStepSeconds = 24.0f / std::sqrt( squareTargetVelocity );
	} else {
		timeStepSeconds = 24.0f / std::sqrt( squarePovVelocity );
	}

	float secondsAhead = 0.0f;
	while( secondsAhead < xerpTimeSeconds ) {
		secondsAhead += timeStepSeconds;

		vec3_t from;
		vec3_t xerpEntOrigin;

		VectorMA( viewOrigin, secondsAhead, povVelocity, from );
		VectorMA( targetEnt->s.origin, secondsAhead, vec3_origin, xerpEntOrigin );

		GetRandomPointInBox( xerpEntOrigin, targetEnt->r.mins, targetEnt->r.size, to );
		if( CastRay( from, to ) ) {
			return false;
		}
	}

	return true;
}

bool SnapVisTable::TryCullingByCastingRays( const edict_t *povEnt, const vec_t *viewOrigin, const edict_t *targetEnt ) {
	const gclient_t *targetClient = targetEnt->r.client;
	if( !targetClient ) {
		return false;
	}

	const gclient_t *povClient = povEnt->r.client;
	if( !povClient ) {
		return false;
	}

	vec3_t clientViewOrigin;
	VectorCopy( povEnt->s.origin, clientViewOrigin );
	clientViewOrigin[2] += povClient->ps.viewheight;

	// Should be an actual origin
	if( !VectorCompare( clientViewOrigin, viewOrigin ) ) {
		return false;
	}

	// After all these checks we can consider the client-to-entity visibility relation symmetrical

	if( int cachedResult = GetExistingResult( povEnt->s.number, targetEnt->s.number ) ) {
		// Return true if invisible
		return cachedResult < 0;
	}

	if( DoCullingByCastingRays( povEnt, viewOrigin, targetEnt ) ) {
		MarkAsInvisible( povEnt->s.number, targetEnt->s.number );
		return true;
	}

	MarkAsVisible( povEnt->s.number, targetEnt->s.number );
	return false;
}