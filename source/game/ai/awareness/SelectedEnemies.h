#ifndef QFUSION_SELECTEDENEMIES_H
#define QFUSION_SELECTEDENEMIES_H

#include "EnemiesTracker.h"
#include "../Selection.h"

class SelectedEnemies: public Selection {
	friend class Bot;

	static const auto MAX_ACTIVE_ENEMIES = AiEnemiesTracker::MAX_ACTIVE_ENEMIES;
	// Selected active enemies are kept in array for these reasons:
	// 1) Being stable relatively to unlinking by the supplying tracker
	// 2) Being able to address precached data by enemy index
	StaticVector<const TrackedEnemy *, MAX_ACTIVE_ENEMIES> enemies;

	const Bot *const bot;
	int64_t timeoutAt { 0 };
	unsigned instanceId { 0 };

	struct alignas( 4 ) FrameCachedFloat {
		mutable Int64Align4 computedAt { -1 };
		mutable float value { std::numeric_limits<float>::infinity() };

		operator const float() const {
			assert( computedAt == level.time );
			return value;
		}

		void Invalidate() { computedAt = -1; }
	};

	struct alignas( 4 ) FrameCachedBool {
		mutable Int64Align4 computedAt { -1 };
		mutable bool value { false };

		operator const bool() const {
			assert( computedAt == level.time );
			return value;
		}

		void Invalidate() { computedAt = -1; }
	};

	struct alignas( 4 ) FrameCachedFloatArray {
		mutable Int64Align4 computedAt { -1 };
		mutable float values[MAX_ACTIVE_ENEMIES];

		operator const float *() const {
			assert( computedAt == level.time );
			return values;
		}

		void Invalidate() { computedAt = -1; }
	};

	// Values are cached independently for every enemy
	mutable FrameCachedFloat threatFactors[MAX_ACTIVE_ENEMIES];
	mutable FrameCachedBool canEnemyHit[MAX_ACTIVE_ENEMIES];

	mutable FrameCachedFloat maxThreatFactor;
	mutable FrameCachedBool canEnemiesHit;
	mutable FrameCachedBool couldHitIfTurns;
	mutable FrameCachedFloatArray botViewDirDotToEnemyDir;
	mutable FrameCachedFloatArray enemyViewDirDotToBotDir;
	mutable FrameCachedBool aboutToHitEBorIG;
	mutable FrameCachedBool aboutToHitLGorPG;
	mutable FrameCachedBool aboutToHitRLorSW;
	mutable FrameCachedBool arePotentiallyHittable;

	void CheckValid( const char *function ) const {
#ifdef _DEBUG
		if( AreValid() ) {
			return;
		}

		char tag[64];
		AI_FailWith( va_r( tag, sizeof( tag ), "SelectedEnemies::%s()", tag ), "Selected enemies are invalid\n" );
#endif
	}

	explicit SelectedEnemies( const Bot *bot_ ) : bot( bot_ ) {}

	bool TestAboutToHitEBorIG( int64_t levelTime ) const;
	bool TestAboutToHitLGorPG( int64_t levelTime ) const;
	bool TestAboutToHitRLorSW( int64_t levelTime ) const;

	bool AreAboutToHit( FrameCachedBool *value, bool ( SelectedEnemies::*testHit )( int64_t ) const ) const {
		auto levelTime = level.time;
		if( levelTime != value->computedAt ) {
			value->value = ( this->*testHit )( levelTime );
			value->computedAt = levelTime;
		}
		return value->value;
	}

	const float *GetBotViewDirDotToEnemyDirValues() const;
	const float *GetEnemyViewDirDotToBotDirValues() const;
public:
	bool AreValid() const;

	bool ValidAsSelection() const override { return AreValid(); }

	void Invalidate();

	void SetToListOfActive( const TrackedEnemy *listHead, unsigned timeout );
	void SetToLostOrHidden( const TrackedEnemy *enemy, unsigned timeout );

	unsigned InstanceId() const override { return instanceId; }

	bool IsPrimaryEnemy( const edict_t *ent ) const {
		return !enemies.empty() && enemies.front()->ent == ent;
	}

	bool IsPrimaryEnemy( const TrackedEnemy *enemy ) const {
		return !enemies.empty() && enemies.front() == enemy;
	}

	Vec3 LastSeenOrigin() const {
		CheckValid( "LastSeenOrigin" );
		return enemies.front()->LastSeenOrigin();
	}

	Vec3 ActualOrigin() const {
		CheckValid( "ActualOrigin" );
		return Vec3( enemies.front()->ent->s.origin );
	}

	Vec3 LastSeenVelocity() const {
		CheckValid( "LastSeenVelocity" );
		return enemies.front()->LastSeenVelocity();
	}

	int64_t LastSeenAt() const {
		CheckValid( "LastSeenAt" );
		return enemies.front()->LastSeenAt();
	}

	Vec3 ClosestEnemyOrigin( const Vec3 &relativelyTo ) const {
		return ClosestEnemyOrigin( relativelyTo.Data() );
	}

	Vec3 ClosestEnemyOrigin( const vec3_t relativelyTo ) const;

	typedef TrackedEnemy::SnapshotsQueue SnapshotsQueue;
	const SnapshotsQueue &LastSeenSnapshots() const {
		CheckValid( "LastSeenSnapshots" );
		return enemies.front()->lastSeenSnapshots;
	}

	Vec3 ActualVelocity() const {
		CheckValid( "ActualVelocity" );
		return Vec3( enemies.front()->ent->velocity );
	}

	Vec3 Mins() const {
		CheckValid( "Mins" );
		return Vec3( enemies.front()->ent->r.mins );
	}

	Vec3 Maxs() const {
		CheckValid( "Maxs" );
		return Vec3( enemies.front()->ent->r.maxs );
	}

	Vec3 LookDir() const;

	Vec3 EnemyAngles() const {
		CheckValid( "EnemyAngles" );
		return Vec3( enemies.front()->ent->s.angles );
	}

	float DamageToKill() const;

	int PendingWeapon() const;

	unsigned FireDelay() const;

	bool IsStaticSpot() const {
		return Ent()->r.client == nullptr;
	}

	const edict_t *Ent() const {
		CheckValid( "Ent" );
		return enemies.front()->ent;
	}

	const edict_t *TraceKey() const {
		CheckValid( "TraceKey" );
		return enemies.front()->ent;
	}

	bool OnGround() const {
		CheckValid( "OnGround" );
		return enemies.front()->ent->groundentity != nullptr;
	}

	bool HaveQuad() const;
	bool HaveCarrier() const;
	bool Contain( const TrackedEnemy *enemy ) const;
	float TotalInflictedDamage() const;

	float MaxDotProductOfBotViewAndDirToEnemy() const {
		auto *dots = GetBotViewDirDotToEnemyDirValues();
		return *std::max_element( dots, dots + enemies.size() );
	}

	float MaxDotProductOfEnemyViewAndDirToBot() const {
		auto *dots = GetEnemyViewDirDotToBotDirValues();
		return *std::max_element( dots, dots + enemies.size() );
	}

	// Checks whether a bot can potentially hit enemies from its origin if it adjusts view angles properly
	bool ArePotentiallyHittable() const;

	typedef const TrackedEnemy **EnemiesIterator;
	inline EnemiesIterator begin() const { return (EnemiesIterator)enemies.cbegin(); }
	inline EnemiesIterator end() const { return (EnemiesIterator)enemies.cend(); }

	bool CanHit() const;
	bool GetCanHit( unsigned enemyNum, float viewDot ) const;
	bool TestCanHit( const edict_t *attacker, const edict_t *victim, float viewDot ) const;

	bool CanBeHit() const;
	bool CouldBeHitIfBotTurns() const;

	bool HaveGoodSniperRangeWeapons() const;
	bool HaveGoodFarRangeWeapons() const;
	bool HaveGoodMiddleRangeWeapons() const;
	bool HaveGoodCloseRangeWeapons() const;

	bool AreAboutToHitEBorIG() const {
		return AreAboutToHit( &aboutToHitEBorIG, &SelectedEnemies::TestAboutToHitEBorIG );
	}

	bool AreAboutToHitRLorSW() const {
		return AreAboutToHit( &aboutToHitRLorSW, &SelectedEnemies::TestAboutToHitRLorSW );
	}

	bool AreAboutToHitLGorPG() const {
		return AreAboutToHit( &aboutToHitLGorPG, &SelectedEnemies::TestAboutToHitLGorPG );
	}

	bool AreThreatening() const {
		CheckValid( "AreThreatening" );
		return MaxThreatFactor() > 0.9f;
	}

	float MaxThreatFactor() const;
	float GetThreatFactor( unsigned enemyNum ) const;
	float ComputeThreatFactor( unsigned enemyNum ) const;
	float ComputeThreatFactor( const edict_t *ent, unsigned *enemyNum = nullptr ) const;
};

#endif
