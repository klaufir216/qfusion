#include "snd_env_effects.h"
#include "snd_effect_sampler.h"
#include "snd_propagation.h"

#include <algorithm>
#include <limits>
#include <cmath>

void Effect::CheckCurrentlyBoundEffect( src_t *src ) {
	ALint effectType;

	// We limit every source to have only a single effect.
	// This is required to comply with the runtime effects count restriction.
	// If the effect type has been changed, we have to delete an existing effect.
	qalGetEffecti( src->effect, AL_EFFECT_TYPE, &effectType );
	if( this->type == effectType ) {
		return;
	}

	// Detach the slot from the source
	qalSource3i( src->source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL );
	// Detach the effect from the slot
	qalAuxiliaryEffectSloti( src->effectSlot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL );

	// TODO: Can we reuse the effect?
	qalDeleteEffects( 1, &src->effect );

	IntiallySetupEffect( src );
}

void Effect::IntiallySetupEffect( src_t *src ) {
	qalGenEffects( 1, &src->effect );
	qalEffecti( src->effect, AL_EFFECT_TYPE, this->type );
}

float Effect::GetMasterGain( src_s *src ) const {
	return src->fvol * src->volumeVar->value;
}

void Effect::AdjustGain( src_t *src ) const {
	qalSourcef( src->source, AL_GAIN, GetMasterGain( src ) );
}

void Effect::AttachEffect( src_t *src ) {
	// Set gain in any case (useful if the "attenuate on obstruction" flag has been turned off).
	AdjustGain( src );

	// Attach the effect to the slot
	qalAuxiliaryEffectSloti( src->effectSlot, AL_EFFECTSLOT_EFFECT, src->effect );
	// Feed the slot from the source
	qalSource3i( src->source, AL_AUXILIARY_SEND_FILTER, src->effectSlot, 0, AL_FILTER_NULL );
}

void UnderwaterFlangerEffect::IntiallySetupEffect( src_t *src ) {
	Effect::IntiallySetupEffect( src );
	// This is the only place where the flanger gets tweaked
	qalEffectf( src->effect, AL_FLANGER_DEPTH, 0.5f );
	qalEffectf( src->effect, AL_FLANGER_FEEDBACK, -0.4f );
}

float UnderwaterFlangerEffect::GetMasterGain( src_t *src ) const {
	float gain = src->fvol * src->volumeVar->value;
	// Lower gain significantly if there is a medium transition
	// (if the listener is not in liquid and the source is, and vice versa)
	if( hasMediumTransition ) {
		gain *= 0.25f;
	}

	// Modify the gain by the direct obstruction factor
	// Lowering the gain by 1/3 on full obstruction is fairly sufficient (its not linearly perceived)
	gain *= 1.0f - 0.33f * directObstruction;
	assert( gain >= 0.0f && gain <= 1.0f );
	return gain;
}

void UnderwaterFlangerEffect::BindOrUpdate( src_t *src ) {
	CheckCurrentlyBoundEffect( src );

	qalFilterf( src->directFilter, AL_LOWPASS_GAINHF, 1.0f - directObstruction );

	AttachEffect( src );
}

float ReverbEffect::GetMasterGain( src_t *src ) const {
	float gain = src->fvol * src->volumeVar->value;

	// Both partial obstruction factors are within [0, 1] range, so we multiply by 0.5
	float obstructionFactor = 0.5f * ( this->directObstruction + this->secondaryRaysObstruction );
	assert( obstructionFactor >= 0.0f && obstructionFactor <= 1.0f );
	// The gain might be lowered up to 2x
	gain *= 1.0f - 0.5f * obstructionFactor;
	assert( gain >= 0.0f && gain <= 1.0f );
	return gain;
}

void StandardReverbEffect::BindOrUpdate( src_t *src ) {
	CheckCurrentlyBoundEffect( src );

	qalEffectf( src->effect, AL_REVERB_DENSITY, this->density );
	qalEffectf( src->effect, AL_REVERB_DIFFUSION, this->diffusion );
	qalEffectf( src->effect, AL_REVERB_GAIN, this->gain );
	qalEffectf( src->effect, AL_REVERB_GAINHF, this->gainHf );
	qalEffectf( src->effect, AL_REVERB_DECAY_TIME, this->decayTime );
	qalEffectf( src->effect, AL_REVERB_REFLECTIONS_GAIN, this->reflectionsGain );
	qalEffectf( src->effect, AL_REVERB_REFLECTIONS_DELAY, this->reflectionsDelay );
	qalEffectf( src->effect, AL_REVERB_LATE_REVERB_GAIN, this->lateReverbGain );
	qalEffectf( src->effect, AL_REVERB_LATE_REVERB_DELAY, this->lateReverbDelay );

	qalFilterf( src->directFilter, AL_LOWPASS_GAINHF, 1.0f - directObstruction );

	AttachEffect( src );
}

void EaxReverbEffect::BindOrUpdate( src_t *src ) {
	CheckCurrentlyBoundEffect( src );

	qalEffectf( src->effect, AL_EAXREVERB_DENSITY, this->density );
	qalEffectf( src->effect, AL_EAXREVERB_DIFFUSION, this->diffusion );
	qalEffectf( src->effect, AL_EAXREVERB_GAIN, this->gain );
	qalEffectf( src->effect, AL_EAXREVERB_GAINHF, this->gainHf );
	qalEffectf( src->effect, AL_EAXREVERB_DECAY_TIME, this->decayTime );
	qalEffectf( src->effect, AL_EAXREVERB_REFLECTIONS_GAIN, this->reflectionsGain );
	qalEffectf( src->effect, AL_EAXREVERB_REFLECTIONS_DELAY, this->reflectionsDelay );
	qalEffectf( src->effect, AL_EAXREVERB_LATE_REVERB_GAIN, this->lateReverbGain );
	qalEffectf( src->effect, AL_EAXREVERB_LATE_REVERB_DELAY, this->lateReverbDelay );

	qalEffectf( src->effect, AL_EAXREVERB_HFREFERENCE, this->hfReference );

	qalEffectf( src->effect, AL_EAXREVERB_ECHO_TIME, this->echoTime );
	qalEffectf( src->effect, AL_EAXREVERB_ECHO_DEPTH, this->echoDepth );

	qalFilterf( src->directFilter, AL_LOWPASS_GAINHF, 1.0f - directObstruction );

	AttachEffect( src );
}

void UnderwaterFlangerEffect::InterpolateProps( const Effect *oldOne, int timeDelta ) {
	const auto *that = Cast<UnderwaterFlangerEffect *>( oldOne );
	if( !that ) {
		return;
	}

	const Interpolator interpolator( timeDelta );
	directObstruction = interpolator( directObstruction, that->directObstruction, 0.0f, 1.0f );
}

void ReverbEffect::CopyReverbProps( const ReverbEffect *that ) {
	// Avoid memcpy... This is not a POD type
	density = that->density;
	diffusion = that->diffusion;
	gain = that->gain;
	gainHf = that->gainHf;
	decayTime = that->decayTime;
	reflectionsGain = that->reflectionsGain;
	reflectionsDelay = that->reflectionsDelay;
	lateReverbGain = that->lateReverbGain;
	lateReverbDelay = that->lateReverbDelay;
	secondaryRaysObstruction = that->secondaryRaysObstruction;
}

bool ReverbEffect::ShouldKeepLingering( float sourceQualityHint, int64_t millisNow ) const {
	if( sourceQualityHint <= 0 ) {
		return false;
	}
	if( millisNow - lastUpdateAt > 200 ) {
		return false;
	}
	clamp_high( sourceQualityHint, 1.0f );
	float factor = 0.5f * sourceQualityHint;
	factor += 0.25f * ( ( 1.0f - directObstruction ) + ( 1.0f - secondaryRaysObstruction ) );
	assert( factor >= 0.0f && factor <= 1.0f );
	return distanceAtLastUpdate < 192.0f + 768.0f * factor;
}

void ReverbEffect::InterpolateCommonReverbProps( const Interpolator &interpolator, const ReverbEffect *that ) {
	directObstruction = interpolator( directObstruction, that->directObstruction, 0.0f, 1.0f );
	density = interpolator( density, that->density, 0.0f, 1.0f );
	diffusion = interpolator( diffusion, that->diffusion, 0.0f, 1.0f );
	gain = interpolator( gain, that->gain, 0.0f, 1.0f );
	gainHf = interpolator( gain, that->gainHf, 0.0f, 1.0f );
	decayTime = interpolator( decayTime, that->decayTime, 0.1f, 20.0f );
	reflectionsGain = interpolator( reflectionsGain, that->reflectionsGain, 0.0f, 3.16f );
	reflectionsDelay = interpolator( reflectionsDelay, that->reflectionsDelay, 0.0f, 0.3f );
	lateReverbGain = interpolator( lateReverbGain, that->lateReverbGain, 0.0f, 10.0f );
	lateReverbDelay = interpolator( lateReverbDelay, that->lateReverbDelay, 0.0f, 0.1f );
	secondaryRaysObstruction = interpolator( secondaryRaysObstruction, that->secondaryRaysObstruction, 0.0f, 1.0f );
}

void StandardReverbEffect::InterpolateProps( const Effect *oldOne, int timeDelta ) {
	if( const auto *that = Cast<ReverbEffect *>( oldOne ) ) {
		InterpolateCommonReverbProps( Interpolator( timeDelta ), that );
	}
}

void EaxReverbEffect::InterpolateProps( const Effect *oldOne, int timeDelta ) {
	const auto *that = Cast<EaxReverbEffect *>( oldOne );
	if( !that ) {
		return;
	}

	Interpolator interpolator( timeDelta );
	InterpolateCommonReverbProps( interpolator, that );
	hfReference = interpolator( hfReference, that->hfReference, 1000.0f, 20000.0f );
	echoTime = interpolator( echoTime, that->echoTime, 0.075f, 0.25f );
	echoDepth = interpolator( echoDepth, that->echoDepth, 0.0f, 1.0f );
}

void EaxReverbEffect::CopyReverbProps( const ReverbEffect *effect ) {
	ReverbEffect::CopyReverbProps( effect );
	if( const auto *that = Cast<EaxReverbEffect *>( effect ) ) {
		this->hfReference = that->hfReference;
		this->echoTime = that->echoTime;
		this->echoDepth = that->echoDepth;
	} else {
		this->hfReference = 5000.0f;
		this->echoTime = 0.25f;
		this->echoDepth = 0.0f;
	}
}

void ChorusEffect::BindOrUpdate( struct src_s *src ) {
	CheckCurrentlyBoundEffect( src );

	qalEffecti( src->effect, AL_CHORUS_PHASE, phase );
	qalEffecti( src->effect, AL_CHORUS_WAVEFORM, waveform );

	qalEffectf( src->effect, AL_CHORUS_DELAY, delay );
	qalEffectf( src->effect, AL_CHORUS_DEPTH, depth );
	qalEffectf( src->effect, AL_CHORUS_RATE, rate );
	qalEffectf( src->effect, AL_CHORUS_FEEDBACK, feedback );

	qalFilterf( src->directFilter, AL_LOWPASS_GAINHF, 1.0f );

	AttachEffect( src );
}

void DistortionEffect::BindOrUpdate( struct src_s *src ) {
	CheckCurrentlyBoundEffect( src );

	qalEffectf( src->effect, AL_DISTORTION_EDGE, edge );
	qalEffectf( src->effect, AL_DISTORTION_EDGE, gain );

	qalFilterf( src->directFilter, AL_LOWPASS_GAINHF, 1.0f );

	AttachEffect( src );
}

void EchoEffect::BindOrUpdate( struct src_s *src ) {
	CheckCurrentlyBoundEffect( src );

	qalEffectf( src->effect, AL_ECHO_DELAY, delay );
	qalEffectf( src->effect, AL_ECHO_LRDELAY, lrDelay );
	qalEffectf( src->effect, AL_ECHO_DAMPING, damping );
	qalEffectf( src->effect, AL_ECHO_FEEDBACK, feedback );
	qalEffectf( src->effect, AL_ECHO_SPREAD, spread );

	qalFilterf( src->directFilter, AL_LOWPASS_GAINHF, 1.0f );

	AttachEffect( src );
}

void EaxReverbEffect::UpdatePanning( src_s *src, const vec3_t listenerOrigin, const mat3_t listenerAxes ) {
	const auto *updateState = &src->panningUpdateState;

	// Unfortunately we have to recompute directions every panning update
	// as the source might have moved and we update panning much more frequently than emission points
	// (TODO: use cached results for non-moving sources?)
	vec3_t reflectionDirs[PanningUpdateState::MAX_POINTS];
	unsigned numReflectionDirs = 0;

	// A weighted sum of directions. Will be used for reflections panning.
	vec3_t reverbPanDir = { 0, 0, 0 };
	for( unsigned i = 0; i < updateState->numPassedSecondaryRays; ++i ) {
		float *dir = &reflectionDirs[numReflectionDirs][0];
		VectorSubtract( listenerOrigin, src->panningUpdateState.reflectionPoints[i], dir );
		float squareDistance = VectorLengthSquared( dir );
		// Do not even take into account directions that have very short segments
		if( squareDistance < 72.0f * 72.0f ) {
			continue;
		}

		numReflectionDirs++;
		const float distance = std::sqrt( squareDistance );
		VectorScale( dir, 1.0f / distance, dir );
		// Store the distance as the 4-th vector component

		// Note: let's apply a factor giving far reflection points direction greater priority.
		// Otherwise the reverb is often panned to a nearest wall and that's totally wrong.
		float factor = 0.3f + 0.7f * ( distance / REVERB_ENV_DISTANCE_THRESHOLD );
		VectorMA( reverbPanDir, factor, dir, reverbPanDir );
	}

	if( numReflectionDirs ) {
		VectorNormalize( reverbPanDir );
	}

	// "If there is an active EaxReverbEffect, setting source origin/velocity is delegated to it".
	UpdateDelegatedSpatialization( src, listenerOrigin );

	vec3_t basePan;
	// Convert to "speakers" coordinate system
	basePan[0] = -DotProduct( reverbPanDir, &listenerAxes[AXIS_RIGHT] );
	// Not sure about "minus" sign in this case...
	// We need something like 9.1 sound system (that has channels distinction in height) to test that
	basePan[1] = -DotProduct( reverbPanDir, &listenerAxes[AXIS_UP] );
	basePan[2] = -DotProduct( reverbPanDir, &listenerAxes[AXIS_FORWARD] );

	float reflectionsPanScale;
	float lateReverbPanScale;
	// Let late reverb be more focused for huge/vast environments
	// Do not pan early reverb for own sounds.
	// Pan early reverb much less for own sounds.
	const float decayFrac = std::min( 1.0f, decayTime / MAX_REVERB_DECAY );
	// If the sound is relative to the listener, lower the panning
	if( src->attenuation == 1.0f ) {
		lateReverbPanScale = 0.25f * decayFrac;
		reflectionsPanScale = 0.0f;
	} else {
		lateReverbPanScale = 0.5f * decayFrac;
		reflectionsPanScale = 0.5f * lateReverbPanScale;
	}

	vec3_t reflectionsPan, lateReverbPan;
	VectorCopy( basePan, reflectionsPan );
	VectorScale( reflectionsPan, reflectionsPanScale, reflectionsPan );
	VectorCopy( basePan, lateReverbPan );
	VectorScale( lateReverbPan, lateReverbPanScale, lateReverbPan );

	qalEffectfv( src->effect, AL_EAXREVERB_REFLECTIONS_PAN, reflectionsPan );
	qalEffectfv( src->effect, AL_EAXREVERB_LATE_REVERB_PAN, lateReverbPan );
}

void EaxReverbEffect::UpdateDelegatedSpatialization( struct src_s *src, const vec3_t listenerOrigin ) {
	if( src->attenuation == ATTN_NONE ) {
		// It MUST already be a relative sound
#ifndef PUBLIC_BUILD
		ALint value;
		qalGetSourcei( src->source, AL_SOURCE_RELATIVE, &value );
		assert( value == AL_TRUE );
#endif
		return;
	}

	qalSourcei( src->source, AL_SOURCE_RELATIVE, AL_FALSE );
	// The velocity is kept untouched for now.
	qalSourcefv( src->source, AL_VELOCITY, src->velocity );

	const float *sourceOrigin = src->origin;
	// Setting effect panning vectors is not sufficient for "realistic" obstruction,
	// as the dry path is still propagates like if there were no obstacles and walls.
	// We try modifying the source origin as well to simulate sound propagation.
	// These conditions must be met:
	// 1) "realistic" obstruction is enabled
	// 2) the direct path is fully obstructed
	// 3) there is a definite indirect precomputed propagation path
	if( s_realistic_obstruction->integer && directObstruction == 1.0f ) {
		// Provide a fake origin for the source that is at the same distance
		// as the real origin and is aligned to the sound propagation "window"
		// TODO: Precache at least the listener leaf for this sound backend update frame
		if( const int listenerLeaf = trap_PointLeafNum( listenerOrigin ) ) {
			if( const int srcLeaf = trap_PointLeafNum( src->origin ) ) {
				vec3_t dir;
				float distance;
				if( PropagationTable::Instance()->GetIndirectPathProps( srcLeaf, listenerLeaf, dir, &distance ) ) {
					// The table stores distance using this granularity, so it might be zero
					// for very close leaves. Adding an extra distance won't harm
					// (even if the indirect path length is already larger than the straight euclidean distance).
					distance += 256.0f;
					// Negate the vector scale multiplier as the dir is an sound influx dir to the listener
					// and we want to shift the origin along the line of the dir but from the listener
					VectorScale( dir, -distance, tmpSourceOrigin );
					// Shift the listener origin in `dir` direction for `distance` units
					VectorAdd( listenerOrigin, tmpSourceOrigin, tmpSourceOrigin );
					// Use the shifted origin as a fake position in the world-space for the source
					sourceOrigin = tmpSourceOrigin;
				}
			}
		}
	}

	qalSourcefv( src->source, AL_POSITION, sourceOrigin );
}