#include "SelectedEnemies.h"
#include "EntitiesPvsCache.h"
#include "../bot.h"

bool SelectedEnemies::AreValid() const {
	for( const auto *enemy: enemies ) {
		if( !enemy->IsValid() ) {
			return false;
		}
	}

	return timeoutAt > level.time;
}

void SelectedEnemies::Invalidate() {
	enemies.clear();
	timeoutAt = 0;

	for( auto &value: threatFactors ) {
		value.Invalidate();
	}
	for( auto &value: canEnemyHit ) {
		value.Invalidate();
	}

	maxThreatFactor.Invalidate();
	canEnemiesHit.Invalidate();
	couldHitIfTurns.Invalidate();
	botViewDirDotToEnemyDir.Invalidate();
	enemyViewDirDotToBotDir.Invalidate();
	aboutToHitEBorIG.Invalidate();
	aboutToHitLGorPG.Invalidate();
	aboutToHitRLorSW.Invalidate();
	arePotentiallyHittable.Invalidate();
}

void SelectedEnemies::SetToListOfActive( const TrackedEnemy *listHead, unsigned timeout ) {
	this->timeoutAt = level.time + timeout;

	assert( enemies.empty() );
	for( const auto *enemy = listHead; enemy; enemy = enemy->NextInActiveList() ) {
		enemies.push_back( enemy );
	}
}

void SelectedEnemies::SetToLostOrHidden( const TrackedEnemy *enemy, unsigned timeout ) {
	this->timeoutAt = level.time + timeout;

	assert( enemies.empty() );
	enemies.push_back( enemy );
}

Vec3 SelectedEnemies::ClosestEnemyOrigin( const vec3_t relativelyTo ) const {
	const TrackedEnemy *closestEnemy = nullptr;
	float minSquareDistance = std::numeric_limits<float>::max();
	for( const auto *enemy: enemies ) {
		float squareDistance = enemy->LastSeenOrigin().SquareDistanceTo( relativelyTo );
		if( minSquareDistance > squareDistance ) {
			minSquareDistance = squareDistance;
			closestEnemy = enemy;
		}
	}

	assert( closestEnemy );
	return closestEnemy->LastSeenOrigin();
}

Vec3 SelectedEnemies::LookDir() const {
	CheckValid( "LookDir" );

	if( const auto *ai = enemies.front()->ent->ai ) {
		// TODO: Once again remove this awkward pointer chasing
		if( const auto *bot = ai->botRef ) {
			return bot->EntityPhysicsState()->ForwardDir();
		}
	}

	vec3_t lookDir;
	AngleVectors( enemies.front()->ent->s.angles, lookDir, nullptr, nullptr );
	return Vec3( lookDir );
}

float SelectedEnemies::DamageToKill() const {
	CheckValid( "DamageToKill" );

	float result = 0.0f;
	for( const auto *enemy: enemies ) {
		float damageToKill = ::DamageToKill( enemy->ent, g_armor_protection->value, g_armor_degradation->value );
		if( enemy->HasShell() ) {
			damageToKill *= 4.0f;
		}
		result += damageToKill;
	}

	return result;
}

int SelectedEnemies::PendingWeapon() const {
	if( const auto *enemy = enemies.front() ) {
		if( const auto *ent = enemy->ent ) {
			if( const auto *client = ent->r.client ) {
				return client->ps.stats[STAT_PENDING_WEAPON];
			}
		}
	}

	return -1;
}

unsigned SelectedEnemies::FireDelay() const {
	unsigned minDelay = std::numeric_limits<unsigned>::max();
	for( const auto *enemy: enemies ) {
		auto delay = enemy->FireDelay();
		if( delay < minDelay ) {
			minDelay = delay;
		}
	}

	return minDelay;
}

bool SelectedEnemies::HaveQuad() const {
	CheckValid( "HaveQuad" );

	for( const auto *enemy: enemies ) {
		if( enemy->HasQuad() ) {
			return true;
		}
	}

	return false;
}

bool SelectedEnemies::HaveCarrier() const {
	CheckValid( "HaveCarrier" );

	for( const auto *enemy: enemies ) {
		if( enemy->IsCarrier() ) {
			return true;
		}
	}

	return false;
}

bool SelectedEnemies::Contain( const TrackedEnemy *enemy ) const {
	CheckValid( "Contain" );

	for( const auto *activeEnemy: enemies ) {
		if( activeEnemy == enemy ) {
			return true;
		}
	}

	return false;
}

float SelectedEnemies::MaxThreatFactor() const {
	const auto levelTime = level.time;
	if( level.time == maxThreatFactor.computedAt ) {
		return maxThreatFactor;
	}

	if( enemies.empty() ) {
		return 0.0f;
	}

	maxThreatFactor.value = 0;
	for( unsigned i = 0; i < enemies.size(); ++i ) {
		float factor = GetThreatFactor( i );
		if( factor > maxThreatFactor.value ) {
			maxThreatFactor.value = factor;
		}
	}

	maxThreatFactor.computedAt = levelTime;
	return maxThreatFactor;
}

float SelectedEnemies::GetThreatFactor( unsigned enemyNum ) const {
	FrameCachedFloat &value = threatFactors[enemyNum];
	if( value.computedAt == level.time ) {
		return value;
	}

	value.computedAt = level.time;
	value.value = ComputeThreatFactor( enemyNum );
	return value;
}

float SelectedEnemies::ComputeThreatFactor( unsigned enemyNum ) const {
	assert( enemyNum >= 0 );
	const auto *enemy = enemies[enemyNum];
	float entFactor = ComputeThreatFactor( enemy->ent, &enemyNum );
	if( level.time - enemy->LastAttackedByTime() < 1000 ) {
		entFactor = Q_Sqrt( entFactor );
	}

	return entFactor;
}

float SelectedEnemies::ComputeThreatFactor( const edict_t *ent, unsigned *enemyNum ) const {
	if( !ent ) {
		return 0.0f;
	}

	// Try cutting off further expensive calls by doing this cheap test first
	if( const auto *client = ent->r.client ) {
		// Can't shoot soon.
		if( client->ps.stats[STAT_WEAPON_TIME] > 800 ) {
			return 0.0f;
		}
	}

	Vec3 enemyToBotDir( bot->Origin() );
	enemyToBotDir -= ent->s.origin;
	enemyToBotDir.NormalizeFast();

	float dot;
	if( enemyNum != nullptr ) {
		// Try reusing this value that is very likely to be cached
		dot = GetEnemyViewDirDotToBotDirValues()[*enemyNum];
	} else {
		vec3_t enemyLookDir;
		AngleVectors( ent->s.angles, enemyLookDir, nullptr, nullptr );
		dot = enemyToBotDir.Dot( enemyLookDir );
	}

	// Check whether the enemy is itself a bot.
	// Check whether the bot is an tracked/selected enemy of other bot?
	// This however would make other bots way too special.
	// The code should work fine for all kind of enemies.
	if( ent->ai && ent->ai->botRef ) {
		if( dot < ent->ai->botRef->FovDotFactor() ) {
			return 0.0f;
		}
	} else if( ent->r.client && dot < 0.2f ) {
		// There is no threat if the bot is not in fov for a client (but not for a turret for example)
		return 0.0f;
	}

	if( !EntitiesPvsCache::Instance()->AreInPvs( ent, game.edicts + bot->EntNum() ) ) {
		return 0.0f;
	}

	if( ent->s.effects & ( EF_QUAD | EF_CARRIER ) ) {
		return 1.0f;
	}

	if( const auto *hazard = bot->PrimaryHazard() ) {
		if( hazard->attacker == ent ) {
			return 0.5f + 0.5f * BoundedFraction( hazard->damage, 75 );
		}
	}

	// Its guaranteed that the enemy cannot hit
	if( dot < 0.7f ) {
		return 0.5f * dot;
	}

	float result = dot;
	// If the enemy belongs to these "selected enemies", try using a probably cached value of the "can hit" test.
	// Otherwise perform a computation (there is no cache for enemies not belonging to this selection)
	if( enemyNum != nullptr ) {
		if( !GetCanHit( *enemyNum, dot ) ) {
			result *= 0.5f;
		}
	} else if( !TestCanHit( ent, game.edicts + bot->EntNum(), dot ) ) {
		result *= 0.5f;
	}

	return Q_Sqrt( result );
}

float SelectedEnemies::TotalInflictedDamage() const {
	CheckValid( "TotalInflictedDamage" );

	float damage = 0.0f;
	for( const auto *activeEnemy: enemies ) {
		damage += activeEnemy->TotalInflictedDamage();
	}

	return damage;
}

bool SelectedEnemies::ArePotentiallyHittable() const {
	CheckValid( "ArePotentiallyHittable" );

	const auto levelTime = level.time;
	if( arePotentiallyHittable.computedAt == levelTime ) {
		return arePotentiallyHittable;
	}

	const auto *__restrict pvsCache = EntitiesPvsCache::Instance();
	const auto *__restrict viewDots = GetBotViewDirDotToEnemyDirValues();
	const auto *__restrict self = game.edicts + bot->EntNum();

	trace_t trace;
	Vec3 viewPoint( self->s.origin );
	viewPoint.Z() += self->viewheight;
	for( unsigned i = 0; i < enemies.size(); ++i ) {
		const auto *enemyEnt = enemies[i]->ent;
		if( !enemyEnt ) {
			continue;
		}
		if( viewDots[i] < 0.7f ) {
			continue;
		}
		if( !pvsCache->AreInPvs( self, enemyEnt ) ) {
			continue;
		}
		SolidWorldTrace( &trace, viewPoint.Data(), enemyEnt->s.origin );
		if( trace.fraction == 1.0f ) {
			arePotentiallyHittable.computedAt = levelTime;
			arePotentiallyHittable.value = true;
			return true;
		}
	}

	arePotentiallyHittable.computedAt = levelTime;
	arePotentiallyHittable.value = false;
	return false;
}

bool SelectedEnemies::CanHit() const {
	CheckValid( "CanHit" );

	const auto *viewDots = GetEnemyViewDirDotToBotDirValues();

	const auto levelTime = level.time;
	if( canEnemiesHit.computedAt == levelTime ) {
		return canEnemiesHit;
	}

	for( unsigned i = 0; i < enemies.size(); ++i ) {
		if( GetCanHit( i, viewDots[i] ) ) {
			canEnemiesHit.computedAt = levelTime;
			canEnemiesHit.value = true;
			return true;
		}
	}

	canEnemiesHit.computedAt = levelTime;
	canEnemiesHit.value = false;
	return false;
}

bool SelectedEnemies::GetCanHit( unsigned enemyNum, float viewDot ) const {
	FrameCachedBool &value = canEnemyHit[enemyNum];
	const auto levelTime = level.time;
	if( levelTime == value.computedAt ) {
		return value;
	}

	value.computedAt = level.time;
	value.value = TestCanHit( enemies[enemyNum]->ent, game.edicts + bot->EntNum(), viewDot );
	return value;
}

bool SelectedEnemies::TestCanHit( const edict_t *attacker, const edict_t *victim, float viewDot ) const {
	if( !( attacker && victim ) ) {
		return false;
	}

	if( viewDot < 0.7f ) {
		return false;
	}

	if( !EntitiesPvsCache::Instance()->AreInPvs( attacker, victim ) ) {
		return false;
	}

	auto *targetEnt = const_cast<edict_t *>( attacker );
	trace_t trace;
	auto *enemyEnt = const_cast<edict_t *>( victim );
	Vec3 traceStart( enemyEnt->s.origin );
	traceStart.Z() += enemyEnt->viewheight;

	G_Trace( &trace, traceStart.Data(), nullptr, nullptr, targetEnt->s.origin, enemyEnt, MASK_AISOLID );
	if( trace.fraction != 1.0f && game.edicts + trace.ent == targetEnt ) {
		return true;
	}

	// If there is a distinct chest point (we call it chest since it is usually on chest position)
	if( std::abs( targetEnt->viewheight ) > 8 ) {
		Vec3 targetPoint( targetEnt->s.origin );
		targetPoint.Z() += targetEnt->viewheight;
		G_Trace( &trace, traceStart.Data(), nullptr, nullptr, targetPoint.Data(), enemyEnt, MASK_AISOLID );
		if( trace.fraction != 1.0f && game.edicts + trace.ent == targetEnt ) {
			return true;
		}
	}

	// Don't waste cycles on further tests (as it used to be).
	// This test is for getting a coarse info anyway.

	return false;
}

bool SelectedEnemies::CouldBeHitIfBotTurns() const {
	CheckValid( "CouldBeHitIfBotTurns" );

	const auto levelTime = level.time;
	if( levelTime == couldHitIfTurns.computedAt ) {
		return couldHitIfTurns;
	}

	// Lets take into account only primary enemy
	couldHitIfTurns.value = TestCanHit( game.edicts + bot->EntNum(), enemies.front()->ent, 1.0f );
	couldHitIfTurns.computedAt = levelTime;
	return couldHitIfTurns;
}

bool SelectedEnemies::CanBeHit() const {
	// Check whether it could be possibly hit from bot origin and the bot is looking at it
	return CouldBeHitIfBotTurns() && GetBotViewDirDotToEnemyDirValues()[0] > bot->FovDotFactor();
}

bool SelectedEnemies::HaveGoodSniperRangeWeapons() const {
	CheckValid( "HaveGoodSniperRangeWeapons" );
	for( const auto *activeEnemy: enemies ) {
		if( activeEnemy->BoltsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->BulletsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->InstasReadyToFireCount() ) {
			return true;
		}
	}
	return false;
}

bool SelectedEnemies::HaveGoodFarRangeWeapons() const {
	CheckValid( "HaveGoodFarRangeWeapons" );
	for( const auto *activeEnemy: enemies ) {
		if( activeEnemy->BoltsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->BulletsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->PlasmasReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->InstasReadyToFireCount() ) {
			return true;
		}
	}
	return false;
}

bool SelectedEnemies::HaveGoodMiddleRangeWeapons() const {
	CheckValid( "HaveGoodMiddleRangeWeapons" );
	for( const auto *activeEnemy: enemies ) {
		if( activeEnemy->RocketsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->LasersReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->PlasmasReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->WavesReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->BulletsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->ShellsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->InstasReadyToFireCount() ) {
			return true;
		}
	}
	return false;
}

bool SelectedEnemies::HaveGoodCloseRangeWeapons() const {
	CheckValid( "HaveGoodCloseRangeWeapons" );
	for( const auto *activeEnemy: enemies ) {
		if( activeEnemy->RocketsReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->PlasmasReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->WavesReadyToFireCount() ) {
			return true;
		}
		if( activeEnemy->ShellsReadyToFireCount() ) {
			return true;
		}
	}
	return false;
}

const float *SelectedEnemies::GetBotViewDirDotToEnemyDirValues() const {
	const auto levelTime = level.time;
	if( levelTime == botViewDirDotToEnemyDir.computedAt ) {
		return botViewDirDotToEnemyDir;
	}

	const float viewHeight = playerbox_stand_viewheight;
	Vec3 botViewDir( bot->EntityPhysicsState()->ForwardDir() );
	for( unsigned i = 0; i < enemies.size(); ++i ) {
		Vec3 botToEnemyDir( enemies[i]->LastSeenOrigin() );
		botToEnemyDir -= bot->Origin();
		botToEnemyDir.Z() -= viewHeight;
		botToEnemyDir.NormalizeFast();
		botViewDirDotToEnemyDir.values[i] = botViewDir.Dot( botToEnemyDir );
	}

	botViewDirDotToEnemyDir.computedAt = levelTime;
	return botViewDirDotToEnemyDir;
}

const float *SelectedEnemies::GetEnemyViewDirDotToBotDirValues() const {
	const auto levelTime = level.time;
	if( levelTime == enemyViewDirDotToBotDir.computedAt ) {
		return enemyViewDirDotToBotDir;
	}

	const float viewHeight = playerbox_stand_viewheight;
	for( unsigned i = 0; i < enemies.size(); ++i ) {
		Vec3 enemyToBotDir( bot->Origin() );
		enemyToBotDir -= enemies[i]->LastSeenOrigin();
		enemyToBotDir.Z() -= viewHeight;
		enemyToBotDir.NormalizeFast();
		enemyViewDirDotToBotDir.values[i] = enemies[i]->LookDir().Dot( enemyToBotDir );
	}

	enemyViewDirDotToBotDir.computedAt = levelTime;
	return enemyViewDirDotToBotDir;
}

bool SelectedEnemies::TestAboutToHitEBorIG( int64_t levelTime ) const {
	const auto *__restrict pvsCache = EntitiesPvsCache::Instance();
	const auto *__restrict viewDots = GetEnemyViewDirDotToBotDirValues();
	const auto *__restrict botOrigin = bot->Origin();

	trace_t trace;
	for( unsigned i = 0; i < enemies.size(); ++i ) {
		const auto *const enemy = enemies[i];
		if( !enemy->IsShootableCurrOrPendingWeapon( WEAP_ELECTROBOLT ) ) {
			if( !enemy->IsShootableCurrOrPendingWeapon( WEAP_INSTAGUN ) ) {
				continue;
			}
		}

		// We can dodge at the last movement, so wait until there is 1/3 of a second to make a shot
		if( enemy->FireDelay() > 333 ) {
			continue;
		}

		// Is not going to put crosshair right now
		// TODO: Check past view dots and derive direction?
		if( viewDots[i] < 0.85f ) {
			continue;
		}

		if( !pvsCache->AreInPvs( game.edicts + bot->EntNum(), enemy->ent ) ) {
			continue;
		}

		const Vec3 enemyOrigin( enemy->LastSeenOrigin() );
		Vec3 traceStart( enemyOrigin );
		traceStart.Z() += playerbox_stand_viewheight;
		SolidWorldTrace( &trace, traceStart.Data(), botOrigin );
		if( trace.fraction != 1.0f ) {
			continue;
		}

		const float squareSpeed = enemy->LastSeenVelocity().SquaredLength();
		// Hitting at this speed is unlikely
		if( squareSpeed > 650 * 650 ) {
			continue;
		}

		const auto *const ent = enemy->ent;
		if( !ent ) {
			// Shouldn't happen?
			continue;
		}

		const auto *const client = ent->r.client;
		if( !client ) {
			return true;
		}

		// If not zooming
		if( !client->ps.stats[PM_STAT_ZOOMTIME] ) {
			const float squareDistance = enemyOrigin.SquareDistanceTo( bot->Origin() );
			// It's unlikely to hit at this distance
			if( squareDistance > 1250 * 1250 ) {
				continue;
			}
		} else {
			// It's hard to hit having a substantial speed while zooming
			if( squareSpeed > 400 * 400 ) {
				continue;
			}
		}
	}

	return false;
}

bool SelectedEnemies::TestAboutToHitLGorPG( int64_t levelTime ) const {
	const auto *__restrict pvsCache = EntitiesPvsCache::Instance();
	const auto *__restrict viewDots = GetEnemyViewDirDotToBotDirValues();
	const auto *__restrict botOrigin = bot->Origin();
	constexpr float squareDistanceThreshold = WorldState::MIDDLE_RANGE_MAX * WorldState::MIDDLE_RANGE_MAX;

	trace_t trace;
	for( unsigned i = 0; i < enemies.size(); ++i ) {
		const auto *const enemy = enemies[i];
		// Skip enemies that are out of LG range. (Consider PG to be inefficient outside of this range too)
		if( enemy->LastSeenOrigin().SquareDistanceTo( botOrigin ) > squareDistanceThreshold ) {
			continue;
		}

		if( !enemy->IsShootableCurrOrPendingWeapon( WEAP_LASERGUN ) ) {
			if( !enemy->IsShootableCurrOrPendingWeapon( WEAP_PLASMAGUN ) ) {
				continue;
			}
		}

		// We can start dodging at the last moment, are not going to be hit hard
		if( enemy->FireDelay() > 333 ) {
			continue;
		}

		// Is not going to put crosshair right now
		if( viewDots[i] < 0.85f ) {
			continue;
		}

		// TODO: Check past view dots and derive direction?

		if( !pvsCache->AreInPvs( game.edicts + bot->EntNum(), enemy->ent ) ) {
			continue;
		}

		Vec3 traceStart( enemy->LastSeenOrigin() );
		traceStart.Z() += playerbox_stand_viewheight;
		SolidWorldTrace( &trace, traceStart.Data(), botOrigin );
		if( trace.fraction == 1.0f ) {
			return true;
		}
	}
	return false;
}

bool SelectedEnemies::TestAboutToHitRLorSW( int64_t levelTime ) const {
	const auto *__restrict pvsCache = EntitiesPvsCache::Instance();
	const auto *__restrict viewDots = GetEnemyViewDirDotToBotDirValues();
	const float *__restrict botOrigin = bot->Origin();

	trace_t trace;
	for( unsigned i = 0; i < enemies.size(); ++i ) {
		const auto *const enemy = enemies[i];

		float distanceThreshold = 512.0f;
		// Ideally should check the bot environment too
		const float deltaZ = enemy->LastSeenOrigin().Z() - botOrigin[2];
		if( deltaZ > 16.0f ) {
			distanceThreshold += 2.0f * BoundedFraction( deltaZ, 128.0f );
		} else if( deltaZ < -16.0f ) {
			distanceThreshold -= BoundedFraction( deltaZ, 128.0f );
		}

		const float squareDistance = enemy->LastSeenOrigin().SquareDistanceTo( botOrigin );
		if( squareDistance > distanceThreshold * distanceThreshold ) {
			continue;
		}

		if( !enemy->IsShootableCurrOrPendingWeapon( WEAP_ROCKETLAUNCHER ) ) {
			if( !enemy->IsShootableCurrOrPendingWeapon( WEAP_SHOCKWAVE ) ) {
				continue;
			}
		}

		const float distance = Q_Sqrt( squareDistance );
		const float distanceFraction = std::min( distance, distanceThreshold ) * Q_Rcp( distanceThreshold );
		// Do not wait for an actual shot on a short distance.
		// Its impossible to dodge on a short distance due to damage splash.
		// If the distance is close to zero 750 millis of reloading left must be used for making a dodge.
		if( enemy->FireDelay() > 750 - ( ( 750 - 333 ) * distanceFraction ) ) {
			continue;
		}

		// Is not going to put crosshair right now
		if( viewDots[i] < 0.3f + 0.4f * distanceFraction ) {
			continue;
		}

		if( !pvsCache->AreInPvs( game.edicts + bot->EntNum(), enemy->ent ) ) {
			continue;
		}

		// TODO: Check view dot and derive direction?
		Vec3 enemyViewOrigin( enemy->LastSeenOrigin() );
		enemyViewOrigin.Z() += playerbox_stand_viewheight;
		SolidWorldTrace( &trace, enemyViewOrigin.Data(), botOrigin );
		if( trace.fraction == 1.0f ) {
			return true;
		}

		// A coarse environment test, check whether there are hittable environment elements
		// around the bot that are visible for the enemy
		for( int x = -1; x <= 1; x += 2 ) {
			for( int y = -1; y <= 1; y += 2 ) {
				Vec3 sidePoint( botOrigin );
				sidePoint.X() += 64.0f * x;
				sidePoint.Y() += 64.0f * y;
				SolidWorldTrace( &trace, botOrigin, sidePoint.Data() );
				if( trace.fraction == 1.0f || ( trace.surfFlags & SURF_NOIMPACT ) ) {
					continue;
				}
				const Vec3 oldImpact( trace.endpos );
				// Notice the order: we trace a ray from enemy to impact point to avoid having to offset start point
				SolidWorldTrace( &trace, enemyViewOrigin.Data(), oldImpact.Data() );
				if( trace.fraction == 1.0f || oldImpact.SquareDistanceTo( trace.endpos ) < 8 * 8 ) {
					return true;
				}
			}
		}
	}

	return false;
}