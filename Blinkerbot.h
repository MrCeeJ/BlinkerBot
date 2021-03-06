#pragma once

#include "sc2api/sc2_interfaces.h"
#include "sc2api/sc2_agent.h"
#include "sc2api/sc2_map_info.h"
#include "sc2lib/sc2_lib.h"
#include "ProductionManager.h"
#include "ArmyManager.h"
#include <iostream>
#include "UnitData.h"
#include <ctime>

using namespace sc2;
enum ArmyStatus { Defend, Attack, Retreat, Regroup };

class BlinkerBot : public sc2::Agent
{
	ProductionManager productionManager;
	ArmyManager armyManager;
	Point2D rallyPoint;

public:
	BlinkerBot();
	virtual void OnGameStart() override;
	virtual void OnStep() override;
    virtual void OnUnitIdle(const Unit *unit) override;
	virtual void OnUnitDestroyed(const Unit *unit) override;
    virtual void OnUnitCreated(const Unit *unit) override;
    virtual void OnUnitEnterVision(const Unit *unit) override;
	virtual void OnBuildingConstructionComplete(const Unit *unit) override;
	virtual void OnUpgradeCompleted(UpgradeID upgrade) override;
};

