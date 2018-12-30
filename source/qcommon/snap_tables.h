#ifndef QFUSION_SNAP_TABLES_H
#define QFUSION_SNAP_TABLES_H

#include "qcommon.h"
#include "snap_write.h"

/**
 * Stores a "shadowed" state of entities for every client.
 * Shadowing an entity means transmission of randomized data
 * for fields that should not be really transmitted but
 * we are forced to transmit some parts of it (that's how the current netcode works).
 * Shadowing has an anti-cheat purpose.
 */
class SnapShadowTable {
	template <typename> friend class SingletonHolder;

	bool *table;

	SnapShadowTable();

	~SnapShadowTable() {
		::free( table );
	}
public:
	static void Init();
	static void Shutdown();
	static SnapShadowTable *Instance();

	void MarkEntityAsShadowed( int playerNum, int targetEntNum ) {
		assert( (unsigned)playerNum < (unsigned)MAX_CLIENTS );
		table[playerNum * MAX_CLIENTS + targetEntNum] = true;
	}

	bool IsEntityShadowed( int playerNum, int targetEntNum ) const {
		assert( (unsigned)playerNum < (unsigned)MAX_CLIENTS );
		return table[playerNum * MAX_CLIENTS + targetEntNum];
	}

	void Clear() {
		::memset( table, 0, ( MAX_CLIENTS ) * ( MAX_EDICTS ) * sizeof( bool ) );
	}
};

/**
 * Stores a visibility state of entities for every client.
 * We try using more aggressive culling of transmitted entities
 * rather than just sending everything in PVS.
 * These tests are not conservative and sometimes are prone to false negatives
 * but real play-tests do not show a noticeable gameplay impact.
 * In worst case a false negative is comparable to a lost packet.
 * For performance reasons only entities that are clients are tested for visibility.
 * An introduction of aggressive transmitted entities visibility culling greatly reduces wallhack utility.
 * Moreover this cached visibility table can be used for various server-side purposes (like AI vision).
 */
class SnapVisTable {
	template <typename> friend class SingletonHolder;

	cmodel_state_t *const cms;
	int8_t *table;
	float collisionWorldRadius;

	explicit SnapVisTable( cmodel_state_t *cms_ );

	bool CastRay( const vec3_t from, const vec3_t to, int topNodeHint );
	bool DoCullingByCastingRays( const edict_t *clientEnt, const vec3_t viewOrigin, const edict_t *targetEnt );

	void MarkCachedResult( int entNum1, int entNum2, bool isVisible ) {
		const int clientNum1 = entNum1 - 1;
		const int clientNum2 = entNum2 - 1;
		assert( (unsigned)clientNum1 < (unsigned)( MAX_CLIENTS ) );
		assert( (unsigned)clientNum2 < (unsigned)( MAX_CLIENTS ) );
		auto value = (int8_t)( isVisible ? +1 : -1 );
		table[clientNum1 * MAX_CLIENTS + clientNum2] = value;
		table[clientNum2 * MAX_CLIENTS + clientNum1] = value;
	}
public:
	static void Init( cmodel_state_t *cms_ );
	static void Shutdown();
	static SnapVisTable *Instance();

	void Clear() {
		memset( table, 0, ( MAX_CLIENTS ) * ( MAX_CLIENTS ) * sizeof( int8_t ) );
	}

	void MarkAsInvisible( int entNum1, int entNum2 ) {
		MarkCachedResult( entNum1, entNum2, false );
	}

	void MarkAsVisible( int entNum1, int entNum2 ) {
		MarkCachedResult( entNum1, entNum2, true );
	}

	int GetExistingResult( int povEntNum, int targetEntNum ) {
		// Check whether they are at least valid entity numbers
		assert( (unsigned)povEntNum < (unsigned)( MAX_EDICTS ) );
		assert( (unsigned)targetEntNum < (unsigned)( MAX_EDICTS ) );
		const int clientNum1 = povEntNum - 1;
		const int clientNum2 = targetEntNum - 1;
		if( (unsigned)clientNum1 >= (unsigned)( MAX_CLIENTS ) ) {
			return 0;
		}
		if( (unsigned)clientNum2 >= (unsigned)( MAX_CLIENTS ) ) {
			return 0;
		}
		return table[clientNum1 * MAX_CLIENTS + clientNum2];
	}

	bool TryCullingByCastingRays( const edict_t *clientEnt, const vec3_t viewOrigin, const edict_t *targetEnt );
};

#endif
