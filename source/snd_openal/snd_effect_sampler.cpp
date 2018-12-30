#include "snd_effect_sampler.h"
#include "snd_leaf_props_cache.h"
#include "snd_effects_allocator.h"
#include "snd_presets_registry.h"
#include "snd_propagation.h"

#include "../gameshared/q_collision.h"

#include <limits>
#include <random>

static UnderwaterFlangerEffectSampler underwaterFlangerEffectSampler;

static ReverbEffectSampler reverbEffectSampler;

Effect *EffectSamplers::TryApply( const ListenerProps &listenerProps, src_t *src, const src_t *tryReusePropsSrc ) {
	Effect *effect;
	if( ( effect = ::underwaterFlangerEffectSampler.TryApply( listenerProps, src, tryReusePropsSrc ) ) ) {
		return effect;
	}
	if( ( effect = ::reverbEffectSampler.TryApply( listenerProps, src, tryReusePropsSrc ) ) ) {
		return effect;
	}

	trap_Error( "Can't find an applicable effect sampler\n" );
}

// We want sampling results to be reproducible especially for leaf sampling and thus use this local implementation
static std::minstd_rand0 samplingRandom;

float EffectSamplers::SamplingRandom() {
	typedef decltype( samplingRandom ) R;
	return ( samplingRandom() - R::min() ) / (float)( R::max() - R::min() );
}

Effect *UnderwaterFlangerEffectSampler::TryApply( const ListenerProps &listenerProps, src_t *src, const src_t * ) {
	if( !listenerProps.isInLiquid && !src->envUpdateState.isInLiquid ) {
		return nullptr;
	}

	float directObstruction = 0.9f;
	if( src->envUpdateState.isInLiquid && listenerProps.isInLiquid ) {
		directObstruction = ComputeDirectObstruction( listenerProps, src );
	}

	auto *effect = EffectsAllocator::Instance()->NewFlangerEffect( src );
	effect->directObstruction = directObstruction;
	effect->hasMediumTransition = src->envUpdateState.isInLiquid ^ listenerProps.isInLiquid;
	return effect;
}

static bool ENV_TryReuseSourceReverbProps( src_t *src, const src_t *tryReusePropsSrc, ReverbEffect *newEffect ) {
	if( !tryReusePropsSrc ) {
		return false;
	}

	auto *reuseEffect = Effect::Cast<const ReverbEffect *>( tryReusePropsSrc->envUpdateState.effect );
	if( !reuseEffect ) {
		return false;
	}

	// We are already sure that both sources are in the same contents kind (non-liquid).
	// Check distance between sources.
	const float squareDistance = DistanceSquared( tryReusePropsSrc->origin, src->origin );
	// If they are way too far for reusing
	if( squareDistance > 96 * 96 ) {
		return false;
	}

	// If they are very close, feel free to just copy props
	if( squareDistance < 4.0f * 4.0f ) {
		newEffect->CopyReverbProps( reuseEffect );
		return true;
	}

	// Do a coarse raycast test between these two sources
	vec3_t start, end, dir;
	VectorSubtract( tryReusePropsSrc->origin, src->origin, dir );
	const float invDistance = 1.0f / sqrtf( squareDistance );
	VectorScale( dir, invDistance, dir );
	// Offset start and end by a dir unit.
	// Ensure start and end are in "air" and not on a brush plane
	VectorAdd( src->origin, dir, start );
	VectorSubtract( tryReusePropsSrc->origin, dir, end );

	trace_t trace;
	trap_Trace( &trace, start, end, vec3_origin, vec3_origin, MASK_SOLID );
	if( trace.fraction != 1.0f ) {
		return false;
	}

	newEffect->CopyReverbProps( reuseEffect );
	return true;
}

void ObstructedEffectSampler::SetupDirectObstructionSamplingProps( src_t *src, unsigned minSamples, unsigned maxSamples ) {
	float quality = s_environment_sampling_quality->value;
	samplingProps_t *props = &src->envUpdateState.directObstructionSamplingProps;

	// If the quality is valid and has not been modified since the pattern has been set
	if( props->quality == quality ) {
		return;
	}

	unsigned numSamples = GetNumSamplesForCurrentQuality( minSamples, maxSamples );

	props->quality = quality;
	props->numSamples = numSamples;
	props->valueIndex = (uint16_t)( EffectSamplers::SamplingRandom() * std::numeric_limits<uint16_t>::max() );
}

struct DirectObstructionOffsetsHolder {
	enum { NUM_VALUES = 256 };
	vec3_t offsets[NUM_VALUES];
	enum { MAX_OFFSET = 20 };

	DirectObstructionOffsetsHolder() {
		for( auto *v: offsets ) {
			for( int i = 0; i < 3; ++i ) {
				v[i] = -MAX_OFFSET + 2 * MAX_OFFSET * EffectSamplers::SamplingRandom();
			}
		}
	}
};

static DirectObstructionOffsetsHolder directObstructionOffsetsHolder;

float ObstructedEffectSampler::ComputeDirectObstruction( const ListenerProps &listenerProps, src_t *src ) {
	trace_t trace;
	envUpdateState_t *updateState;
	float *originOffset;
	vec3_t testedListenerOrigin;
	vec3_t testedSourceOrigin;
	float squareDistance;
	unsigned numTestedRays, numPassedRays;
	unsigned valueIndex;

	updateState = &src->envUpdateState;

	VectorCopy( listenerProps.origin, testedListenerOrigin );
	// TODO: We assume standard view height
	testedListenerOrigin[2] += 18.0f;

	squareDistance = DistanceSquared( testedListenerOrigin, src->origin );
	// Shortcut for sounds relative to the player
	if( squareDistance < 32.0f * 32.0f ) {
		return 0.0f;
	}

	if( !trap_LeafsInPVS( listenerProps.GetLeafNum(), trap_PointLeafNum( src->origin ) ) ) {
		return 1.0f;
	}

	vec3_t hintBounds[2];
	ClearBounds( hintBounds[0], hintBounds[1] );
	AddPointToBounds( testedListenerOrigin, hintBounds[0], hintBounds[1] );
	AddPointToBounds( src->origin, hintBounds[0], hintBounds[1] );
	// Account for obstruction sampling offsets
	// as we are going to compute the top node hint once
	for( int i = 0; i < 3; ++i ) {
		hintBounds[0][i] -= DirectObstructionOffsetsHolder::MAX_OFFSET;
		hintBounds[1][i] += DirectObstructionOffsetsHolder::MAX_OFFSET;
	}

	const int topNodeHint = trap_FindTopNodeForBox( hintBounds[0], hintBounds[1] );
	trap_Trace( &trace, testedListenerOrigin, src->origin, vec3_origin, vec3_origin, MASK_SOLID, topNodeHint );
	if( trace.fraction == 1.0f && !trace.startsolid ) {
		// Consider zero obstruction in this case
		return 0.0f;
	}

	SetupDirectObstructionSamplingProps( src, 3, MAX_DIRECT_OBSTRUCTION_SAMPLES );

	numPassedRays = 0;
	numTestedRays = updateState->directObstructionSamplingProps.numSamples;
	valueIndex = updateState->directObstructionSamplingProps.valueIndex;
	for( unsigned i = 0; i < numTestedRays; i++ ) {
		valueIndex = ( valueIndex + 1 ) % DirectObstructionOffsetsHolder::NUM_VALUES;
		originOffset = directObstructionOffsetsHolder.offsets[ valueIndex ];

		VectorAdd( src->origin, originOffset, testedSourceOrigin );
		trap_Trace( &trace, testedListenerOrigin, testedSourceOrigin, vec3_origin, vec3_origin, MASK_SOLID, topNodeHint );
		if( trace.fraction == 1.0f && !trace.startsolid ) {
			numPassedRays++;
		}
	}

	return 1.0f - 0.9f * ( numPassedRays / (float)numTestedRays );
}

Effect *ReverbEffectSampler::TryApply( const ListenerProps &listenerProps, src_t *src, const src_t *tryReusePropsSrc ) {
	ReverbEffect *effect = EffectsAllocator::Instance()->NewReverbEffect( src );
	effect->directObstruction = ComputeDirectObstruction( listenerProps, src );
	// We try reuse props only for reverberation effects
	// since reverberation effects sampling is extremely expensive.
	// Moreover, direct obstruction reuse is just not valid,
	// since even a small origin difference completely changes it.
	if( ENV_TryReuseSourceReverbProps( src, tryReusePropsSrc, effect ) ) {
		src->envUpdateState.needsInterpolation = false;
	} else {
		ComputeReverberation( listenerProps, src, effect );
	}
	return effect;
}

float ReverbEffectSampler::GetEmissionRadius() const {
	// Do not even bother casting rays 999999 units ahead for very attenuated sources.
	// However, clamp/normalize the hit distance using the same defined threshold
	float attenuation = src->attenuation;

	if( attenuation <= 1.0f ) {
		return 999999.9f;
	}

	clamp_high( attenuation, 10.0f );
	float distance = 4.0f * REVERB_ENV_DISTANCE_THRESHOLD;
	distance -= 3.5f * SQRTFAST( attenuation / 10.0f ) * REVERB_ENV_DISTANCE_THRESHOLD;
	return distance;
}

void ReverbEffectSampler::ResetMutableState( const ListenerProps &listenerProps_, src_t *src_, ReverbEffect *effect_ ) {
	this->listenerProps = &listenerProps_;
	this->src = src_;
	this->effect = effect_;

	GenericRaycastSampler::ResetMutableState( primaryRayDirs, reflectionPoints, primaryHitDistances, src->origin );

	VectorCopy( listenerProps_.origin, testedListenerOrigin );
	testedListenerOrigin[2] += 18.0f;
}

void ReverbEffectSampler::ComputeReverberation( const ListenerProps &listenerProps_,
												src_t *src_,
												ReverbEffect *effect_ ) {
	ResetMutableState( listenerProps_, src_, effect_ );

	// A "realistic obstruction" requires a higher amount of secondary (and thus primary) rays
	if( s_realistic_obstruction->integer ) {
		numPrimaryRays = GetNumSamplesForCurrentQuality( 16, MAX_REVERB_PRIMARY_RAY_SAMPLES );
	} else {
		numPrimaryRays = GetNumSamplesForCurrentQuality( 16, MAX_REVERB_PRIMARY_RAY_SAMPLES / 2 );
	}

	SetupPrimaryRayDirs();

	EmitPrimaryRays();

	if( !numPrimaryHits ) {
		SetMinimalReverbProps();
		return;
	}

	ProcessPrimaryEmissionResults();
	EmitSecondaryRays();
}

void ReverbEffectSampler::SetupPrimaryRayDirs() {
	assert( numPrimaryRays );

	SetupSamplingRayDirs( primaryRayDirs, numPrimaryRays );
}

void ReverbEffectSampler::ProcessPrimaryEmissionResults() {
	// Instead of trying to compute these factors every sampling call,
	// reuse pre-computed properties of CM map leafs that briefly resemble rooms/convex volumes.
	assert( src->envUpdateState.leafNum >= 0 );

	const auto *const leafPropsCache = LeafPropsCache::Instance();
	// Try checking whether there is a preset defined for the leaf.
	// Few notes:
	// 1) Presets are not utilized as-is. Some fields like reference frequencies are unused.
	// 2) Even if we use a preset we still need a secondary emission
	// for an actual reflections pan, and thus a primary emission too.
	if( const auto *presetHandle = leafPropsCache->GetPresetForLeaf( src->envUpdateState.leafNum ) ) {
		effect->ReusePreset( presetHandle );
		return;
	}

	const LeafProps &leafProps = leafPropsCache->GetPropsForLeaf( src->envUpdateState.leafNum );

	const float roomSizeFactor = leafProps.RoomSizeFactor();
	const float metalFactor = leafProps.MetalFactor();
	const float skyFactor = leafProps.SkyFactor();

	// It should be default.
	// Secondary rays obstruction is the only modulation we apply.
	// See EmitSecondaryRays()
	effect->gain = 0.32f;

	// The density must be within [0.0, 1.0] range.
	// Lower the density is, more tinny and metallic a sound appear.
	// Values below 0.3 behave way too artificial.
	effect->density = 1.0f - 0.7f * metalFactor;

	// The diffusion must be within [0.0, 1.0] range.
	// Lowering diffusing has an effect that is similar to echoes that quickly change their panning from left to right.
	// (its probably uses some phase modulation as well).
	effect->diffusion = 1.0f;
	// Apply a non-standard diffusion only for an outdoor environment
	if( skyFactor ) {
		// Make sure the diffusion kicks in only for a really huge space
		effect->diffusion -= roomSizeFactor * roomSizeFactor;
	}

	// The decay time should be within [0.1, 20.0] range.
	// A reverberation starts being really heard from values greater than 0.5.
	constexpr auto maxDecay = EaxReverbEffect::MAX_REVERB_DECAY;
	// This is a minimal decay chosen for tiny rooms
	constexpr auto minDecay = 0.6f;
	// This is an additional decay time that is added on an outdoor environment
	constexpr auto skyExtraDecay = 1.0f;
	static_assert( maxDecay > minDecay + skyExtraDecay, "" );

	effect->decayTime = minDecay + ( maxDecay - minDecay - skyExtraDecay ) * roomSizeFactor + skyExtraDecay * skyFactor;
	assert( effect->decayTime <= maxDecay );

	// The late reverberation gain affects effect strength a lot.
	// It must be within [0.0, 10.0] range.
	// We should really limit us to values below 1.0, preferably even closer to 0.1..0.2 for a generic environment.
	// Higher values can feel "right" for a "cinematic" scene, but are really annoying for an actual in-game experience.

	// This is a base value for huge spaces
	const float distantGain = 0.055f - 0.015f * skyFactor;
	// Let's try doing "energy preservation": an increased decay should lead to decreased gain.
	// These formulae do not have any theoretical foundations but feels good.
	// The `decayFrac` is close to 0 for tiny rooms/short decay and is close to 1 for huge rooms/long decay
	const float decayFrac = ( effect->decayTime - minDecay ) / ( maxDecay - minDecay );
	// This gain factor should be close to 1 for tiny rooms and quickly fall down to almost 0
	const float gainFactorForRoomSize = std::pow( 1.0f - decayFrac, 5.0f );
	effect->lateReverbGain = distantGain + 0.8f * gainFactorForRoomSize;

	// This is an early reverberation gain and it should decay quickly with increasing room size.
	// The values must be within [0.0, 3.16] range.
	// Lets raise early reverberation for metal environment to simulate "live" surfaces.
	effect->reflectionsGain = 0.05f + ( 0.25f + 0.25f * metalFactor ) * gainFactorForRoomSize;

	// Must be within [0.0, 0.3] range.
	// Keep it default... it's hard to tweak
	effect->reflectionsDelay = 0.007f;
	// This is the only exception ... a hack that feels great:
	// Increase the reflections delay for indirect propagation using the propagation path length
	vec3_t tableDir;
	float tableDistance;
	int listenerLeafNum = listenerProps->GetLeafNum();
	const auto *const table = PropagationTable::Instance();
	if( table->GetIndirectPathProps( src->envUpdateState.leafNum, listenerLeafNum, tableDir, &tableDistance ) ) {
		// 2^16 is the maximal distance that may be stored in the table, everything above is clamped
		effect->reflectionsDelay = 0.299f * std::pow( tableDistance / (float)( 1 << 16 ), 0.33f );
	}

	// Must be within [0.0 ... 0.1] range
	effect->lateReverbDelay = 0.011f + 0.088f * roomSizeFactor;

	if( auto *eaxEffect = Effect::Cast<EaxReverbEffect *>( effect ) ) {
		// We have added support for altering HF-reference frequency which has been overlooked
		// and has a great impact on a "clarity" of reverberation shifting a dominant HF frequency in an effect output.
		// It should really be controlled by surface properties.
		// However currently there are no surface hint flags for it.
		// SURF_DUST might seem suitable for it but currently an overwhelming majority of surfaces has it for no reasons.
		// This heuristic works satisfiable:
		// 1) Open spaces should have lesser HF-reference frequency to simulate HF absorption
		// 2) Huge closed spaces should have greater HF-reference as the sound output
		// starts resembling what is expected from a huge cave or a huge hangar for a raised HF-reference.

		// The HF reference must be within [1000, 20000] range
		const float minHfRef = 3500.0f - 1000.0f * skyFactor;
		const float maxHfRef = 7500.0f - 3000.0f * skyFactor;
		eaxEffect->hfReference = minHfRef + ( maxHfRef - minHfRef ) * roomSizeFactor;

		// Apply an echo but only for open spaces
		if( !skyFactor ) {
			eaxEffect->echoTime = 0.25f;
			// Efficiently disable the echo
			eaxEffect->echoDepth = 0.0f;
			return;
		}

		// Must be within [0.075, 0.25] range.
		// We are not sure whether this is still valid for updated OpenAL SOFT versions,
		// but the most strong and distinct echo was for the value of 0.125.
		eaxEffect->echoTime = 0.075f + 0.125f * roomSizeFactor;
		// The echo depth must be within [0.0, 1.0] range.
		// Raise echo depth until sky factor reaches 0.5f, then lower it.
		// So echo depth is within [0.25f, 0.5f] bounds and reaches its maximum at skyFactor = 0.5f
		if( skyFactor < 0.5f ) {
			eaxEffect->echoDepth = 0.25f + 0.5f * 2.0f * skyFactor;
		} else {
			eaxEffect->echoDepth = 0.75f - 0.3f * 2.0f * ( skyFactor - 0.5f );
		}
	}
}

void ReverbEffectSampler::SetMinimalReverbProps() {
	effect->gain = 0.1f;
	effect->density = 1.0f;
	effect->diffusion = 1.0f;
	effect->decayTime = 0.60f;
	effect->reflectionsGain = 0.05f;
	effect->reflectionsDelay = 0.007f;
	effect->lateReverbGain = 0.15f;
	effect->lateReverbDelay = 0.011f;
	effect->gainHf = 0.0f;
	if( auto *eaxEffect = Effect::Cast<EaxReverbEffect *>( effect ) ) {
		eaxEffect->hfReference = 5000.0f;
		eaxEffect->echoTime = 0.25f;
		eaxEffect->echoDepth = 0.0f;
	}
}

void ReverbEffectSampler::EmitSecondaryRays() {
	int listenerLeafNum = listenerProps->GetLeafNum();

	auto *const eaxEffect = Effect::Cast<EaxReverbEffect *>( effect );
	auto *const panningUpdateState = &src->panningUpdateState;

	trace_t trace;

	unsigned numPassedSecondaryRays = 0;
	if( eaxEffect ) {
		panningUpdateState->numPassedSecondaryRays = 0;
		for( unsigned i = 0; i < numPrimaryHits; i++ ) {
			// Cut off by PVS system early, we are not interested in actual ray hit points contrary to the primary emission.
			if( !trap_LeafsInPVS( listenerLeafNum, trap_PointLeafNum( reflectionPoints[i] ) ) ) {
				continue;
			}

			trap_Trace( &trace, reflectionPoints[i], testedListenerOrigin, vec3_origin, vec3_origin, MASK_SOLID );
			if( trace.fraction == 1.0f && !trace.startsolid ) {
				numPassedSecondaryRays++;
				float *savedPoint = panningUpdateState->reflectionPoints[panningUpdateState->numPassedSecondaryRays++];
				VectorCopy( reflectionPoints[i], savedPoint );
			}
		}
	} else {
		for( unsigned i = 0; i < numPrimaryHits; i++ ) {
			if( !trap_LeafsInPVS( listenerLeafNum, trap_PointLeafNum( reflectionPoints[i] ) ) ) {
				continue;
			}

			trap_Trace( &trace, reflectionPoints[i], testedListenerOrigin, vec3_origin, vec3_origin, MASK_SOLID );
			if( trace.fraction == 1.0f && !trace.startsolid ) {
				numPassedSecondaryRays++;
			}
		}
	}

	if( numPrimaryHits ) {
		float frac = numPassedSecondaryRays / (float)numPrimaryHits;
		// The secondary rays obstruction is complement to the `frac`
		effect->secondaryRaysObstruction = 1.0f - frac;
		const auto *const leafPropsCache = LeafPropsCache::Instance();
		// Check whether there were a preset defined for the leaf.
		// In this case do not modify the HF gain which has been already set from a preset.
		if( !leafPropsCache->GetPresetForLeaf( src->envUpdateState.leafNum ) ) {
			// A absence of a HF attenuation sounds poor, metallic/reflective environments should be the only exception.
			const LeafProps &leafProps = leafPropsCache->GetPropsForLeaf( src->envUpdateState.leafNum );
			effect->gainHf = ( 0.4f + 0.5f * leafProps.MetalFactor() ) * frac;
		}
		// We also modify effect gain by a fraction of secondary rays passed to listener.
		// This is not right in theory, but is inevitable in the current game sound model
		// where you can hear across the level through solid walls
		// in order to avoid messy echoes coming from everywhere.
		effect->gain *= 0.75f + 0.25f * frac;
	} else {
		// Set minimal feasible values
		effect->secondaryRaysObstruction = 1.0f;
		effect->gainHf = 0.0f;
		effect->gain *= 0.5f;
	}
}