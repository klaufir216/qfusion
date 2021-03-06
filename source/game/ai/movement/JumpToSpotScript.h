#ifndef QFUSION_JUMPTOSPOTFALLBACK_H
#define QFUSION_JUMPTOSPOTFALLBACK_H

#include "MovementScript.h"

class JumpToSpotScript: public MovementScript {
protected:
	vec3_t targetOrigin { 0, 0, 0 };
	vec3_t startOrigin { 0, 0, 0 };
	unsigned timeout { 0 };
	float reachRadius { 0.0f };
	float startAirAccelFrac { 0.0f };
	float endAirAccelFrac { 0.0f };
	float jumpBoostSpeed { 0.0f };
	bool hasAppliedJumpBoost { false };
	bool allowCorrection { false };
public:
	int undesiredAasContents { AREACONTENTS_LAVA | AREACONTENTS_SLIME | AREACONTENTS_DONOTENTER };
	int undesiredAasFlags { AREA_DISABLED };
	int desiredAasContents { 0 };
	int desiredAasFlags { AREA_GROUNDED };

	JumpToSpotScript( const Bot *bot_, BotMovementModule *module_ )
		: MovementScript( bot_, module_, COLOR_RGB( 255, 0, 128 ) ) {}

	void Activate( const vec3_t startOrigin_,
				   const vec3_t targetOrigin_,
				   unsigned timeout,
				   float reachRadius_ = 32.0f,
				   float startAirAccelFrac_ = 0.0f,
				   float endAirAccelFrac_ = 0.0f,
				   float jumpBoostSpeed_ = 0.0f );

	bool TryDeactivate( MovementPredictionContext *context = nullptr ) override;

	void SetupMovement( MovementPredictionContext *context ) override;
};

#endif
