#ifndef QFUSION_BOT_BRAIN_H
#define QFUSION_BOT_BRAIN_H

#include <stdarg.h>
#include "../ai_base_ai.h"
#include "Planner.h"
#include "../awareness/EnemiesTracker.h"
#include "ItemsSelector.h"
#include "../combat/WeaponSelector.h"
#include "Actions.h"
#include "Goals.h"

struct Hazard;

class BotPlanner : public AiPlanner {
	friend class BotPlanningModule;
	friend class BotItemsSelector;

	Bot *const bot;
	BotPlanningModule *const module;

	StaticVector<BotScriptGoal, Ai::MAX_PLANNER_GOALS> scriptGoals;
	StaticVector<BotScriptAction, Ai::MAX_PLANNER_ACTIONS> scriptActions;

	BotGoal *GetGoalByName( const char *name );
	BotAction *GetActionByName( const char *name );

	BotScriptGoal *AllocScriptGoal() { return scriptGoals.unsafe_grow_back(); }
	BotScriptAction *AllocScriptAction() { return scriptActions.unsafe_grow_back(); }

	const int *Inventory() const;

	template <int Weapon>
	int AmmoReadyToFireCount() const {
		if( !Inventory()[Weapon] ) {
			return 0;
		}
		return Inventory()[WeaponAmmo < Weapon > ::strongAmmoTag] + Inventory()[WeaponAmmo < Weapon > ::weakAmmoTag];
	}

	int ShellsReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_RIOTGUN>(); }
	int GrenadesReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_GRENADELAUNCHER>(); }
	int RocketsReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_ROCKETLAUNCHER>(); }
	int PlasmasReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_PLASMAGUN>(); }
	int BulletsReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_MACHINEGUN>(); }
	int LasersReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_LASERGUN>(); }
	int BoltsReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_ELECTROBOLT>(); }
	int WavesReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_SHOCKWAVE>(); }
	int InstasReadyToFireCount() const { return AmmoReadyToFireCount<WEAP_INSTAGUN>(); }

	bool FindDodgeHazardSpot( const Hazard &hazard, vec3_t spotOrigin );

	void PrepareCurrWorldState( WorldState *worldState ) override;

	bool ShouldSkipPlanning() const override;

	void BeforePlanning() override;
public:
	BotPlanner() = delete;
	// Disable copying and moving
	BotPlanner( BotPlanner &&that ) = delete;

	// A WorldState cached from the moment of last world state update
	WorldState cachedWorldState;

	BotPlanner( Bot *bot_, BotPlanningModule *module_ );
};

#endif
