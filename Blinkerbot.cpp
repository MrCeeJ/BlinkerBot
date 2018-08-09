#include "Blinkerbot.h"
#include "sc2api/sc2_api.h"

using namespace sc2;

BlinkerBot::BlinkerBot(): productionManager(*this), armyManager(*this)
{
}


void BlinkerBot::OnGameStart()
{
	
	for (auto unit : Observation()->GetUnits())
	{
		if (unit->unit_type == UNIT_TYPEID::PROTOSS_PROBE)
		{
			productionManager.addWorker(unit);
		}
		else if (unit->unit_type == UNIT_TYPEID::PROTOSS_NEXUS)
		{
			productionManager.addStructure(unit);
		}
	}
	productionManager.initialise();
	armyManager.initialise();
}

void BlinkerBot::OnStep()
{
	//timer stuff
	std::clock_t start;
	double duration;
	start = std::clock();
	//timer stuff

	productionManager.onStep();
	productionManager.receiveAttackSignal(armyManager.sendAttackSignal());
	productionManager.receiveCloakSignal(armyManager.detectionRequired());
	armyManager.onStep();
	armyManager.receiveRallyPoint(productionManager.getRallyPoint());	

	//timer stuff
	duration = (std::clock() - start) / (double)CLOCKS_PER_SEC * 1000;
	if (duration > 20)
	{
		std::cout << "frame duration: " << duration << '\n';
	}
	//timer stuff
}

void BlinkerBot::OnUnitDestroyed(const sc2::Unit *unit)
{
	if (UnitData::isOurs(unit))
	{
		productionManager.onUnitDestroyed(unit);
		armyManager.removeUnit(unit);
	}
	else
	{
		if (UnitData::isStructure(unit))
		{
			armyManager.removeEnemyStructure(unit);
			if (UnitData::isTownHall(unit))
			{
				productionManager.removeEnemyBase(unit);
			}
		}
		else
		{
			armyManager.removeEnemyUnit(unit);
		}
	}
}
void BlinkerBot::OnUnitEnterVision(const sc2::Unit *unit)
{
	if (UnitData::isStructure(unit))
	{
		armyManager.addEnemyStructure(unit);
		if (UnitData::isTownHall(unit))
		{
			productionManager.addEnemyBase(unit);
		}
	}
	else
	{
		armyManager.addEnemyUnit(unit);
	}
}

void BlinkerBot::OnBuildingConstructionComplete(const sc2::Unit* unit)
{
	productionManager.addStructure(unit);
}

void BlinkerBot::OnUnitCreated(const sc2::Unit *unit)
{
	if ((unit->unit_type == UNIT_TYPEID::PROTOSS_ZEALOT) 
		|| (unit->unit_type == UNIT_TYPEID::PROTOSS_STALKER) 
		|| (unit->unit_type == UNIT_TYPEID::PROTOSS_OBSERVER)
		|| (unit->unit_type == UNIT_TYPEID::PROTOSS_COLOSSUS)
		|| (unit->unit_type == UNIT_TYPEID::PROTOSS_IMMORTAL))
	{
		armyManager.addUnit(unit);
	}
	else
	{
		productionManager.addNewUnit(unit);
	}
	
}

void BlinkerBot::OnUnitIdle(const sc2::Unit *unit)
{
	if (unit->unit_type == UNIT_TYPEID::PROTOSS_PROBE)
	{
		productionManager.returnToMining(unit);
	}
}

void BlinkerBot::OnUpgradeCompleted(UpgradeID upgrade)
{
	productionManager.onUpgradeComplete(upgrade);
}

