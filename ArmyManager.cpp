#include "ArmyManager.h"
#include "Blinkerbot.h"

ArmyManager::ArmyManager(BlinkerBot & bot) : blinkerBot(bot), currentStatus(Retreat), 
regroupComplete(true), warpgateTech(false), beingRushed(false), zerglingSpeed(false), blinkTech(false), 
proxy(false), extendedThermalLanceTech(false), demolitionDuty(false),
regroupStarted(0), currentArmyValue(0), currentEnemyArmyValue(0), totalZerglingSupply(0), 
zerglingTimer(nullptr, 0, 0, Point2D(0, 0), false) {}

void ArmyManager::initialise()
{
	//find potential enemy start locations
	for (auto location : blinkerBot.Observation()->GetGameInfo().enemy_start_locations)
	{
		unexploredEnemyStartLocations.push_back(location);
	}

	//find the enemy race
	for (auto player : blinkerBot.Observation()->GetGameInfo().player_info)
	{
		if (player.player_id != blinkerBot.Observation()->GetPlayerID())
		{
			enemyRace = player.race_requested;
		}
	}
}

void ArmyManager::onStep()
{
	/*
	//timer stuff
	std::clock_t start;
	double duration;
	start = std::clock();
	//timer stuff
	*/

	//printDebug();
	darkTemplarHarass();
	feedback();
	moveObservers();
	checkForZerglingSpeed();

	if (blinkerBot.Observation()->GetGameLoop() % 8 == 0)
	{
		forcefield();
		psistorm();
	}

	const Unit *threatened = underAttack();
	if (threatened)
	{
		//attack(threatened->pos);
		defend(threatened->pos);
		currentStatus = Defend;
		regroupStarted = 0;
	}
	else if (canAttack() && regroupComplete)
	{
		attack();
		currentStatus = Attack;
		regroupStarted = 0;
	}
	else
	{
		if (regroupStarted == 0)
		{
			regroupStarted = blinkerBot.Observation()->GetGameLoop();
		}
		regroupComplete = regroup();
		currentStatus = Regroup;
	}

	/*
	//timer stuff
	duration = (std::clock() - start) / (double)CLOCKS_PER_SEC * 1000;
	if (duration > 30)
	{
		std::cout << "ArmyManager duration: " << duration << '\n';
	}
	//timer stuff
	*/
}

/*
checks if any enemy units are near our bases and returns a unit pointer to the base that is under threat
*/
const Unit *ArmyManager::underAttack()
{
	for (auto enemy : enemyArmy)
	{
		if (enemy->unit_type != UNIT_TYPEID::ZERG_OVERLORD)
		{
			for (auto unit : blinkerBot.Observation()->GetUnits())
			{
				//if there is an enemy in range of one of our structures
				if (UnitData::isOurs(unit) && UnitData::isStructure(unit) &&
					(enemy->last_seen_game_loop == blinkerBot.Observation()->GetGameLoop() &&
					(inRange(enemy, unit) || Distance2D(enemy->pos, unit->pos) < LOCALRADIUS)))
				{
					//filter out proxy pylons
					const Unit *base = getClosestBase(unit);
					if (base && Distance2D(base->pos, unit->pos) < 20)
					{
						if (!(enemy->is_flying && !includes(UNIT_TYPEID::PROTOSS_STALKER)))
						{
							//std::cerr << UnitTypeToName(unit->unit_type) << " is under attack from " << UnitTypeToName(enemy->unit_type) << std::endl;
							return unit;
						}
					}
				}
			}
		}
	}
	return nullptr;
}

/*
issues a regroup command to all of our units. Returns true when all units are within a certain distance of the rally point.
*/
bool ArmyManager::regroup()
{
	if (army.size() > 0)
	{
		//if regroup is taking too long then trigger it so we can attack even if a unit got stuck
		int currentLoop = blinkerBot.Observation()->GetGameLoop();
		if (currentLoop - regroupStarted > WAITTIME)
		{
			return true;
		}

		//tell units to go to the regroup point
		bool unitsCloseEnough = true;
		for (auto armyUnit : army)
		{

			const Unit *enemy = getClosestEnemy(armyUnit.unit->pos);
			/*
			//if zealots get caught by speedlings then they might as well just fight
			if (zerglingSpeed && enemy && enemy->unit_type == UNIT_TYPEID::ZERG_ZERGLING &&
				armyUnit.unit->unit_type == UNIT_TYPEID::PROTOSS_ZEALOT && Distance2D(enemy->pos, armyUnit.unit->pos) < 2 
				&& (armyUnit.unit->orders.empty() || armyUnit.unit->orders.front().ability_id != ABILITY_ID::ATTACK))
			{
				blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, enemy);
			}
			*/
			//else 
				if (armyUnit.unit->orders.empty() || armyUnit.unit->orders.front().ability_id != ABILITY_ID::MOVE ||
				(armyUnit.unit->orders.front().target_pos.x != rallyPoint.x && armyUnit.unit->orders.front().target_pos.y != rallyPoint.y))
			{
				if (proxy)
				{
					if (Distance2D(armyUnit.unit->pos, rallyPoint) > 4 && !(zerglingSpeed && enemy && enemy->unit_type == UNIT_TYPEID::ZERG_ZERGLING &&
						armyUnit.unit->unit_type == UNIT_TYPEID::PROTOSS_ZEALOT && Distance2D(enemy->pos, armyUnit.unit->pos) < 2))
					{
						unitsCloseEnough = false;
						blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::MOVE, rallyPoint);
					}
				}
				else
				{ 
					if (Distance2D(armyUnit.unit->pos, rallyPoint) > LOCALRADIUS)
					{
						unitsCloseEnough = false;
						blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::MOVE, rallyPoint);
					}
				}
			}

			//blink stalkers out of trouble
			if (armyUnit.unit->unit_type == UNIT_TYPEID::PROTOSS_STALKER)
			{
				const Unit *enemy = getClosestEnemy(armyUnit.unit);

				if (enemy && Distance2D(armyUnit.unit->pos, enemy->pos) < LOCALRADIUS
					&& Distance2D(armyUnit.unit->pos, rallyPoint) > LOCALRADIUS)
				{
					blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::EFFECT_BLINK, rallyPoint);
				}
			}
		}
		return unitsCloseEnough;
	}
	else
	{
		//std::cerr << "no units to regroup" << std::endl;
		return false;
	}
}

/*
finds an appropriate point for a unit to attack towards. Returns a random location if no enemy bases can be found
*/
Point2D ArmyManager::findAttackTarget(const Unit *unit)
{
	Point2D target;

	if (enemyStructures.empty())
	{
		//if we haven't scouted the enemy start locations yet, go to those
		if (!unexploredEnemyStartLocations.empty())
		{
			target = *unexploredEnemyStartLocations.begin();
		}
		//if we've been to the enemy main but don't know the location of any enemy bases, set a random location
		else
		{
			target = Point2D(float(GetRandomInteger(0, blinkerBot.Observation()->GetGameInfo().width)), float(GetRandomInteger(0, blinkerBot.Observation()->GetGameInfo().height)));
		}
	}
	//otherwise let's find his closest base and go there
	else
	{
		const Unit *base = getClosestEnemyBase(unit);
		if (base)
		{
			target = base->pos;
		}
	}

	return target;
}

/*
Issues an attack command for our whole army. Units will attack towards enemy start locations until the enemy base is found.
If no enemy bases can be found then units will begin searching randomly around the map until one is located.
When units come into contact with enemies, blink() and kite() will be called.
*/
void ArmyManager::attack()
{
	//if we are walled in, break it before issuing attack commands
	if (demolitionDuty)
	{
		return;
	}
	//check if we can blink on top of our enemy
	if (!army.empty())
	{
		//aggressiveBlink(findAttackTarget(army.front().unit));
	}
	//other micro
	for (auto armyUnit : army)
	{
		//if (!(armyUnit.unit->unit_type == UNIT_TYPEID::PROTOSS_SENTRY && enemyRace == Race::Zerg && !blinkTech))
		{
			//find an appropriate target
			Point2D target = findAttackTarget(armyUnit.unit);

			//if we don't know where the enemy base is, let's search randomly with our idle units
			if (enemyStructures.empty())
			{
				//if we still have unexplored enemy bases, then check and remove any we can see
				if (!unexploredEnemyStartLocations.empty())
				{
					for (std::vector<Point2D>::iterator enemyStartLocation = unexploredEnemyStartLocations.begin();
						enemyStartLocation != unexploredEnemyStartLocations.end();)
					{
						if (Distance2D(armyUnit.unit->pos, *enemyStartLocation) <
							blinkerBot.Observation()->GetUnitTypeData()[armyUnit.unit->unit_type].sight_range)
						{
							enemyStartLocation = unexploredEnemyStartLocations.erase(enemyStartLocation);
						}
						else
						{
							++enemyStartLocation;
						}
					}
				}

				//attack towards either an enemy start location or a random point if we've already seen all the start locations
				if (armyUnit.unit->orders.empty())
				{
					blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, target);
				}
			}
			else
			{

				float averageDistance = averageUnitDistanceToEnemyBase();
				bool retreating = false;
				//if any nearby units are trying to kite, let's run back too so we don't get in their way
				/*
				for (auto otherUnit : army)
				{
					if (otherUnit.unit != armyUnit.unit && otherUnit.status == Retreat
						&& Distance2D(armyUnit.unit->pos, otherUnit.unit->pos) < 8 && armyUnit.unit->weapon_cooldown > 0)
					{
						retreating = true;
					}
				}
				*/
				//we want our unit to run away if either:
				//- nearby retreating units have triggered the retreating flag
				//- our units are spread out causing some units to arrive much earlier than others
				if (retreating || ((armyUnit.unit->weapon_cooldown > 0 ||
					getClosestEnemy(armyUnit.unit) && !inRange(armyUnit.unit, getClosestEnemy(armyUnit.unit))) &&
					(calculateSupplyInRadius(armyUnit.unit->pos, enemyArmy) + calculateEnemyStaticDefenceInRadius(armyUnit.unit->pos)) >
					calculateSupplyInRadius(armyUnit.unit->pos, army)))
				{
					blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::MOVE, rallyPoint);
				}
				else
				{
					bool alreadyAttacking = false;
					if (armyUnit.unit->orders.size() > 0 && armyUnit.unit->orders.front().ability_id == ABILITY_ID::ATTACK
						&& ((*armyUnit.unit->orders.begin()).target_pos.x == target.x && (*armyUnit.unit->orders.begin()).target_pos.y == target.y))
					{
						alreadyAttacking = true;
					}
					//attempt stalker micro
					if (armyUnit.unit->unit_type == UNIT_TYPEID::PROTOSS_STALKER)
					{
						if (!escapeAOE(armyUnit) && !blink(armyUnit.unit) && !kite(armyUnit) && !alreadyAttacking)
						{
							armyUnit.status = Attack;
							blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, target);
						}
					}
					else
					{
						//get the voidrays chargin'
						if (armyUnit.unit->unit_type == UNIT_TYPEID::PROTOSS_VOIDRAY)
						{
							activatePrismaticAlignment(armyUnit.unit);
						}
						//do some micro
						if (!escapeAOE(armyUnit) && !kite(armyUnit) && !alreadyAttacking)
						{
							armyUnit.status = Attack;
							blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, target);
						}
					}
				}
			}
		}
	}
}

/*
returns the average distance of our army units to the enemy base
*/
float ArmyManager::averageUnitDistanceToEnemyBase()
{
	float distance = 0;

	if (enemyStructures.empty())
	{
		return distance;
	}

	for (auto armyUnit : army)
	{
		const Unit *enemyBase = getClosestEnemyBase(armyUnit.unit);
		if (enemyBase)
		{
			distance += Distance2D(armyUnit.unit->pos, enemyBase->pos);
		}
	}

	distance = distance / army.size();

	return distance;
}

/*
tells all our units to attack move towards a specific point on the map.
blink() and kite() will be called when our army comes into contact with enemy units.
*/
void ArmyManager::attack(Point2D target)
{
	//check if we can blink on top of our enemy, otherwise micro as normal
	aggressiveBlink(target);
	for (auto armyUnit : army)
	{
		const Unit *targetUnit = getClosestEnemy(armyUnit.unit->pos);

		if (armyUnit.unit->unit_type == UNIT_TYPEID::PROTOSS_STALKER)
		{
			if (!escapeAOE(armyUnit) && !blink(armyUnit.unit) && !kite(armyUnit))
			{
				armyUnit.status = Attack;
				if (armyUnit.unit->orders.empty() || (*armyUnit.unit->orders.begin()).ability_id != ABILITY_ID::ATTACK)
					//|| ((*armyUnit.unit->orders.begin()).target_pos.x != target.x && (*armyUnit.unit->orders.begin()).target_pos.y != target.y))
				{
					if (targetUnit)
					{
						blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, targetUnit->pos);
					}
					else
					{
						blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, target);
					}
				}
			}
		}
		else
		{
			//get the voidrays chargin'
			if (armyUnit.unit->unit_type == UNIT_TYPEID::PROTOSS_VOIDRAY)
			{
				activatePrismaticAlignment(armyUnit.unit);
			}

			if (!escapeAOE(armyUnit) && !kite(armyUnit))
			{
				armyUnit.status = Attack;
				if (armyUnit.unit->orders.empty() || (*armyUnit.unit->orders.begin()).ability_id != ABILITY_ID::ATTACK)
					//|| ((*armyUnit.unit->orders.begin()).target_pos.x != target.x && (*armyUnit.unit->orders.begin()).target_pos.y != target.y))
				{
					if (targetUnit)
					{
						blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, targetUnit->pos);
					}
					else
					{
						blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, target);
					}
				}
			}
		}
	}
}

void ArmyManager::defend(Point2D threatened)
{
	//check if we need to be defending the top of our ramp
	bool holdRamp = false;
	float mainHeight = blinkerBot.Observation()->GetStartLocation().z;
	if (bases.size() < 2)
	{
		holdRamp = true;
	}

	//check if we can blink on top of our enemy, otherwise micro as normal
	//aggressiveBlink(threatened);

	for (auto armyUnit : army)
	{
		const Unit *targetUnit = getClosestEnemy(armyUnit.unit->pos);

		//if we're defending our main ramp, then units that go too far need to return to the top
		if (holdRamp && mainHeight - armyUnit.unit->pos.z > 0.5 && armyUnit.unit->weapon_cooldown > 0 &&
			(armyUnit.unit->orders.empty() || armyUnit.unit->orders.front().ability_id != ABILITY_ID::MOVE))
		{
			//std::cout << mainHeight << " vs " << armyUnit.unit->pos.z << std::endl;
			//std::cout << "returning unit to top of ramp" << std::endl;
			blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::MOVE, rallyPoint);
		}
		else if (armyUnit.unit->unit_type == UNIT_TYPEID::PROTOSS_STALKER)
		{
			if (!escapeAOE(armyUnit) && !blink(armyUnit.unit) && !kite(armyUnit))
			{
				armyUnit.status = Attack;
				if (armyUnit.unit->orders.empty() || (*armyUnit.unit->orders.begin()).ability_id != ABILITY_ID::ATTACK)
				{
					if (targetUnit && Distance2D(targetUnit->pos, threatened) < LOCALRADIUS)
					{
						blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, targetUnit->pos);
					}
					else
					{
						blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, threatened);
					}
				}
			}
		}
		else if (armyUnit.unit->unit_type == UNIT_TYPEID::PROTOSS_ZEALOT)
		{
			if (!escapeAOE(armyUnit))
			{
				armyUnit.status = Attack;
				if (armyUnit.unit->orders.empty() || 
					((*armyUnit.unit->orders.begin()).ability_id != ABILITY_ID::ATTACK &&
					(*armyUnit.unit->orders.begin()).ability_id != ABILITY_ID::MOVE))
				{
					if (targetUnit && Distance2D(targetUnit->pos, threatened) < LOCALRADIUS)
					{
						blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, targetUnit->pos);
					}
					else
					{
						blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, threatened);
					}
				}
			}
		}
		else
		{
			//get the voidrays chargin'
			if (armyUnit.unit->unit_type == UNIT_TYPEID::PROTOSS_VOIDRAY)
			{
				activatePrismaticAlignment(armyUnit.unit);
			}

			if (!escapeAOE(armyUnit) && !kite(armyUnit))
			{
				armyUnit.status = Attack;
				if (armyUnit.unit->orders.empty() || (*armyUnit.unit->orders.begin()).ability_id != ABILITY_ID::ATTACK)
				{
					if (targetUnit && Distance2D(targetUnit->pos, threatened) < LOCALRADIUS)
					{
						blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, targetUnit->pos);
					}
					else
					{
						blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, threatened);
					}
				}
			}
		}
	}
}

/*
issues a move command back to our rally point for all army units.
*/
void ArmyManager::retreat()
{
	for (auto armyUnit : army)
	{
		blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::MOVE, rallyPoint);

		//blink stalkers out of trouble
		if (armyUnit.unit->unit_type == UNIT_TYPEID::PROTOSS_STALKER)
		{
			const Unit *enemy = getClosestEnemy(armyUnit.unit);

			if (enemy && Distance2D(armyUnit.unit->pos, enemy->pos) < LOCALRADIUS
				&& Distance2D(armyUnit.unit->pos, rallyPoint) > LOCALRADIUS)
			{
				blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::EFFECT_BLINK, rallyPoint);
			}
		}
	}
}

/*
adds newly created units to the appropriate group
*/
void ArmyManager::addUnit(const Unit *unit)
{
	if (unit->unit_type == UNIT_TYPEID::PROTOSS_DARKTEMPLAR)
	{
		darkTemplars.insert(unit);
	}
	else if (unit->unit_type == UNIT_TYPEID::PROTOSS_OBSERVER)
	{
		observers.insert(unit);
	}
	else if (unit->unit_type == UNIT_TYPEID::PROTOSS_PHOTONCANNON)
	{
		photonCannons.insert(unit);
	}
	else if (unit->unit_type == UNIT_TYPEID::PROTOSS_ZEALOT ||
		unit->unit_type == UNIT_TYPEID::PROTOSS_STALKER ||
		unit->unit_type == UNIT_TYPEID::PROTOSS_SENTRY ||
		unit->unit_type == UNIT_TYPEID::PROTOSS_COLOSSUS ||
		unit->unit_type == UNIT_TYPEID::PROTOSS_HIGHTEMPLAR ||
		unit->unit_type == UNIT_TYPEID::PROTOSS_IMMORTAL ||
		unit->unit_type == UNIT_TYPEID::PROTOSS_VOIDRAY)
	{
		for (auto armyUnit : army)
		{
			if (armyUnit.unit == unit)
			{
				return;
			}
		}
		army.push_back(ArmyUnit(unit, Attack));
		//update our army value

		currentArmyValue += blinkerBot.Observation()->GetUnitTypeData()[unit->unit_type].food_required;

		//printDebug();

		if (unit->unit_type == UNIT_TYPEID::PROTOSS_HIGHTEMPLAR)
		{
			highTemplars.insert(unit);
		}
		else if (unit->unit_type == UNIT_TYPEID::PROTOSS_SENTRY)
		{
			sentries.insert(unit);
		}

		if (currentStatus == Regroup || currentStatus == Retreat)
		{
			blinkerBot.Actions()->UnitCommand(unit, ABILITY_ID::MOVE, rallyPoint);
		}
	}
	else if (UnitData::isTownHall(unit))
	{
		bases.insert(unit);
	}
}

/*
removes dead units from their groups
*/
void ArmyManager::removeUnit(const Unit *unit)
{
	if (unit->unit_type == UNIT_TYPEID::PROTOSS_DARKTEMPLAR)
	{
		darkTemplars.erase(unit);
	}
	else if (unit->unit_type == UNIT_TYPEID::PROTOSS_OBSERVER)
	{
		observers.erase(unit);
	}
	else if (unit->unit_type == UNIT_TYPEID::PROTOSS_PHOTONCANNON)
	{
		photonCannons.erase(unit);
	}
	else if (UnitData::isTownHall(unit))
	{
		bases.erase(unit);
	}
	else
	{
		for (std::vector<ArmyUnit>::iterator armyUnit = army.begin(); armyUnit != army.end();)
		{
			if ((*armyUnit).unit == unit)
			{
				armyUnit = army.erase(armyUnit);
				//update army value
				currentArmyValue -= blinkerBot.Observation()->GetUnitTypeData()[unit->unit_type].food_required;
				//printDebug();
				if (unit->unit_type == UNIT_TYPEID::PROTOSS_HIGHTEMPLAR)
				{
					highTemplars.erase(unit);
				}
				else if (unit->unit_type == UNIT_TYPEID::PROTOSS_SENTRY)
				{
					sentries.erase(unit);
				}
			}
			else
			{
				++armyUnit;
			}
		}
	}
}

/*
When an enemy unit becomes visible, let's check if we've seen it before.
If not, add it to the set so we can keep track of army size.
*/
void ArmyManager::addEnemyUnit(const Unit *unit)
{
	if (UnitData::isNeutralRock(unit))
	{
		return;
	}

	bool alreadySeen = false;
	for (auto enemyUnit : enemyArmy)
	{
		if (enemyUnit == unit)
		{
			alreadySeen = true;
		}
	}
	if (!alreadySeen && !UnitData::isStructure(unit)) //&& !UnitData::isWorker(unit)
	{
		//std::cerr << "adding new " << blinkerBot.Observation()->GetUnitTypeData()[unit->unit_type].name << std::endl;
		enemyArmy.insert(unit);
		//update enemy army value
		if (!UnitData::isWorker(unit))
		{
			if (unit->unit_type == UNIT_TYPEID::ZERG_ZERGLING)
			{
				currentEnemyArmyValue += zerglingSupply;
				totalZerglingSupply += zerglingSupply;
			}
			else
			{
				currentEnemyArmyValue += blinkerBot.Observation()->GetUnitTypeData()[unit->unit_type].food_required;
			}
			//printDebug();
		}
	}
}

/*
when we kill an enemy unit, we wanna update the set of active enemies
*/
void ArmyManager::removeEnemyUnit(const Unit *unit)
{
	for (std::set<const Unit *>::iterator enemyUnit = enemyArmy.begin(); enemyUnit != enemyArmy.end();)
	{
		if ((*enemyUnit) == unit)
		{
			enemyArmy.erase(*enemyUnit++);
			if (!UnitData::isWorker(unit))
			{
				if (unit->unit_type == UNIT_TYPEID::ZERG_ZERGLING)
				{
					currentEnemyArmyValue -= zerglingSupply;
					totalZerglingSupply -= zerglingSupply;
				}
				else
				{
					currentEnemyArmyValue -= blinkerBot.Observation()->GetUnitTypeData()[unit->unit_type].food_required;
				}
				//printDebug();
			}
		}
		else
		{
			++enemyUnit;
		}
	}
}

/*
generates an integer representing the size of the threat that enemy static defence represents
*/
int ArmyManager::calculateEnemyStaticDefence()
{
	int total = 0;
	for (auto structure : enemyStructures)
	{
		if (structure->build_progress == 1.0)
		{
			if (structure->unit_type == UNIT_TYPEID::TERRAN_BUNKER ||
				structure->unit_type == UNIT_TYPEID::PROTOSS_PHOTONCANNON ||
				structure->unit_type == UNIT_TYPEID::ZERG_SPINECRAWLER)
			{
				total += 6;
			}
			else if (structure->unit_type == UNIT_TYPEID::TERRAN_PLANETARYFORTRESS)
			{
				total += 20;
			}
		}
	}
	return total;
}

/*
returns true if we have enough units to attack
*/
bool ArmyManager::canAttack()
{
	//attack can be delayed by zergling speed (until blink), rushes (until early game timer runs out), or army size
	if (!(proxy && army.size() < 8) &&
		blinkTech &&
		!beingRushed && currentArmyValue > 1 && 
		((currentArmyValue >= currentEnemyArmyValue)  || blinkerBot.Observation()->GetFoodUsed() > 180))
	{
		return true;
	}
	else if (checkForProxies())
	{
		return true;
	}
	else
	{
		return false;
	}
}

/*
returns our current ArmyStatus
*/
ArmyStatus ArmyManager::getArmyStatus()
{
	return currentStatus;
}

/*
calculates the supply of a given army within the radius of a given point (Unit * version)
*/
float ArmyManager::calculateSupplyInRadius(Point2D centre, std::set<const Unit *> army)
{
	float total = 0;
	for (auto unit : army)
	{
		if (!UnitData::isWorker(unit) && unit->last_seen_game_loop + 200 > blinkerBot.Observation()->GetGameLoop() 
			&& Distance2D(unit->pos, centre) < LOCALRADIUS)
		{
			total += blinkerBot.Observation()->GetUnitTypeData()[unit->unit_type].food_required;
		}
	}
	return total;
}

/*
calculates the supply of a given army within the radius of a given point (ArmyUnit version)
*/
float ArmyManager::calculateSupplyInRadius(Point2D centre, std::vector<ArmyUnit> army)
{
	float total = 0;
	for (auto armyUnit : army)
	{
		if (!UnitData::isWorker(armyUnit.unit) && armyUnit.unit->last_seen_game_loop + 200 > blinkerBot.Observation()->GetGameLoop()
			&& Distance2D(armyUnit.unit->pos, centre) < LOCALRADIUS)
		{
			total += blinkerBot.Observation()->GetUnitTypeData()[armyUnit.unit->unit_type].food_required;
		}
	}
	return total;
}

/*
calculates the supply of an army and workers within the radius of a given point (Unit * version)
*/
float ArmyManager::calculateSupplyAndWorkersInRadius(Point2D centre, std::set<const Unit *> army)
{
	float total = 0;
	for (auto unit : army)
	{
		if (unit->last_seen_game_loop + 30 > blinkerBot.Observation()->GetGameLoop()
			&& Distance2D(unit->pos, centre) < LOCALRADIUS)
		{
			total += blinkerBot.Observation()->GetUnitTypeData()[unit->unit_type].food_required;
		}
	}
	return total;
}

/*
calculates the supply used by a given set of unit pointers
*/
float ArmyManager::calculateSupply(std::set<const Unit *> army)
{
	float total = 0;
	for (auto unit : army)
	{
		//filter out workers
		if (!UnitData::isWorker(unit))
		{
			//if we don't have any colossus or voidrays then we can filter out any units that can't attack ground
			if (!includes(UNIT_TYPEID::PROTOSS_VOIDRAY) && !includes(UNIT_TYPEID::PROTOSS_COLOSSUS))
			{
				if (UnitData::canAttackGround(unit->unit_type))
				{
					total += blinkerBot.Observation()->GetUnitTypeData()[unit->unit_type].food_required;
				}
			}
			//if we are making colossus or voidray then count units that attack air too
			else
			{
				total += blinkerBot.Observation()->GetUnitTypeData()[unit->unit_type].food_required;
			}
		}
	}
	return total;
}

/*
calculates the supply used by a given set of ArmyUnits
*/
float ArmyManager::calculateSupply(std::vector<ArmyUnit> army)
{
	float total = 0;
	for (auto armyUnit : army)
	{
		if (!UnitData::isWorker(armyUnit.unit))
		{
			total += blinkerBot.Observation()->GetUnitTypeData()[armyUnit.unit->unit_type].food_required;
		}
	}
	return total;
}

/*
A forward pylon is used as a rally point. The position is determined by ProductionManager and passed via BlinkerBot.
*/
void ArmyManager::setRallyPoint(Point2D point)
{
	rallyPoint = point;

	//in the event that we have a forward pylon that we are using as a point to retreat to or regroup at...
	if ((currentStatus == Retreat || currentStatus == Regroup) &&
		(getClosestBase(rallyPoint) && Distance2D(getClosestBase(rallyPoint)->pos, rallyPoint) > 20))
	{
		//...if the enemy army is approaching the rally point, we don't want to fight...
		if (getClosestEnemy(rallyPoint) && Distance2D(getClosestEnemy(rallyPoint)->pos, rallyPoint) < 10)
		{
			//...so let's retreat to our closest base
			rallyPoint = getClosestBase(rallyPoint)->pos;
		}
	}
}

/*
checks if a unit can kite vs the closest enemy, and issue a move command if it can
*/
bool ArmyManager::kite(ArmyUnit armyUnit)
{
	//check if we are in range and if we outrange the enemy
	bool canKite = false;
	for (auto enemy : enemyArmy)
	{
		if (!UnitData::isStructure(enemy) && inRange(armyUnit.unit, enemy) && outranges(armyUnit.unit, enemy) && 
			UnitData::canTarget(armyUnit.unit, enemy) && UnitData::canTarget(enemy, armyUnit.unit))
		{
			canKite = true;
		}
	}
	//if the enemy is kitable and we are on cooldown, run away
	if (canKite && armyUnit.unit->weapon_cooldown > 0)
	{
		armyUnit.status = Retreat;
		blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::MOVE, getRetreatPoint(armyUnit.unit));
		return true;
	}
	return false;
}

/*
returns true if attacker outranges target, and false otherwise
*/
bool ArmyManager::outranges(const Unit *attacker, const Unit *target)
{
	if (!attacker || !target || blinkerBot.Observation()->GetUnitTypeData()[attacker->unit_type].weapons.size() == 0 
		|| blinkerBot.Observation()->GetUnitTypeData()[target->unit_type].weapons.size() == 0 || UnitData::isStructure(target)
		|| target->unit_type == UNIT_TYPEID::ZERG_LARVA || target->unit_type == UNIT_TYPEID::ZERG_EGG)
	{
		return false;
	}
	else if ((*blinkerBot.Observation()->GetUnitTypeData()[attacker->unit_type].weapons.begin()).range >
		(*blinkerBot.Observation()->GetUnitTypeData()[target->unit_type].weapons.begin()).range)
	{
		return true;
	}
	else
	{
		return false;
	}
}

/*
issues a defensive blink command backwards away from the closest enemy in the event that shields are low
*/
bool ArmyManager::blink(const Unit *unit)
{
	//check if any enemies are in range of our unit and if they can take out our shields
	bool threat = false;
	for (auto enemy : enemyArmy)
	{
		if (inRange(enemy, unit) && shieldsCritical(unit, enemy))
		{
			threat = true;
		}
	}
	if (threat && unit->unit_type == UNIT_TYPEID::PROTOSS_STALKER)
	{
		blinkerBot.Actions()->UnitCommand(unit, ABILITY_ID::EFFECT_BLINK_STALKER, getRetreatPoint(unit));
		return true;
	}
	return false;
}

/*
find the closest enemy army unit to a given unit.
Ignores flying units if the unit cannot attack flyers, also ignores untargetable units and changelings
*/
const Unit *ArmyManager::getClosestEnemy(const Unit *ourUnit)
{
	if (enemyArmy.size() > 0)
	{
		const Unit *closestEnemy = *enemyArmy.begin();
		for (auto enemy : enemyArmy)
		{
			if (Distance2D(enemy->pos, ourUnit->pos) < Distance2D(closestEnemy->pos, ourUnit->pos) 
				&& UnitData::canTarget(ourUnit, enemy) && UnitData::isVisible(enemy))
			{
			closestEnemy = enemy;
			}
		}
		return closestEnemy;
	}
	else
	{
		return nullptr;
	}
}

/*
find the closest enemy army unit to a given point.
Since this version is not supplied an attacking unit, we cannot check if flying units are attackable
*/
const Unit *ArmyManager::getClosestEnemy(Point2D point)
{
	if (enemyArmy.size() > 0)
	{
		const Unit *closestEnemy = *enemyArmy.begin();
		for (auto enemy : enemyArmy)
		{
			if (Distance2D(enemy->pos, point) < Distance2D(closestEnemy->pos, point)
				&& !UnitData::isChangeling(enemy->unit_type) && UnitData::isTargetable(enemy->unit_type)
				&& UnitData::isVisible(enemy))
			{
				closestEnemy = enemy;
			}
		}
		return closestEnemy;
	}
	else
	{
		return nullptr;
	}
}

/*
find the closest enemy structure to a given unit.
Ignores burrowed creep tumors.
*/
const Unit *ArmyManager::getClosestEnemyBase(const Unit *ourUnit)
{
	if (enemyStructures.empty())
	{
		return nullptr;
	}
	else
	{
		//find the closest structure to our unit
		const Unit *closestStructure = *enemyStructures.begin();
		for (auto structure : enemyStructures)
		{
			if (Distance2D(ourUnit->pos, structure->pos) < Distance2D(ourUnit->pos, closestStructure->pos)
				&& structure->unit_type != UNIT_TYPEID::ZERG_CREEPTUMORBURROWED)
			{
				closestStructure = structure;
			}
		}

		//if the position is within our sight range, let's check that the structure is actually still there
		if ((Distance2D(ourUnit->pos, closestStructure->pos) < blinkerBot.Observation()->GetUnitTypeData()[ourUnit->unit_type].sight_range))
		{
			bool found = false;
			for (auto unit : blinkerBot.Observation()->GetUnits())
			{
				if (unit->tag == closestStructure->tag)
				{
					found = true;
				}
			}
			//if we can't find it then remove it from the set (it might've been destroyed or moved)
			if (!found)
			{
				removeEnemyStructure(closestStructure);
			}
		}

		return closestStructure;
	}
}

/*
returns true when the attacker is in range of the target point
*/
bool ArmyManager::inRange(const Unit *attacker, Point2D target)
{
	//make sure we have an attacker and a target
	if (!attacker || blinkerBot.Observation()->GetUnitTypeData()[attacker->unit_type].weapons.size() == 0)
	{
		return false;
	}
	else if (attacker->unit_type == UNIT_TYPEID::PROTOSS_COLOSSUS && extendedThermalLanceTech)
	{
		float range = blinkerBot.Observation()->GetUnitTypeData()[attacker->unit_type].weapons.front().range + 2;
		if (range >= Distance2D(attacker->pos, target))
		{
			return true;
		}
		else
		{
			return false;
		}
	}
	//check if the attacker is in range with their main weapon
	else if (blinkerBot.Observation()->GetUnitTypeData()[attacker->unit_type].weapons.front().range >= Distance2D(attacker->pos, target))
	{
		return true;
	}
	//try to dodge melee attacks
	else if (blinkerBot.Observation()->GetUnitTypeData()[attacker->unit_type].weapons.front().range <= 1 &&
		Distance2D(attacker->pos, target) <= 2)
	{
		return true;
	}
	else
	{
		return false;
	}
}

/*
returns true when the attacker is in range of the target unit
*/
bool ArmyManager::inRange(const Unit *attacker, const Unit *target)
{
	//make sure we have an attacker and a target
	if (!attacker || !target || blinkerBot.Observation()->GetUnitTypeData()[attacker->unit_type].weapons.size() == 0)
	{
		return false;
	}
	//check if the attacker is in range with their main weapon
	else if (blinkerBot.Observation()->GetUnitTypeData()[attacker->unit_type].weapons.front().range >= Distance2D(attacker->pos, target->pos))
	{
		if (blinkerBot.Observation()->GetGameLoop() % 30 == 0)
		{
		//	std::cerr << UnitTypeToName(attacker->unit_type) << " range " << blinkerBot.Observation()->GetUnitTypeData()[attacker->unit_type].weapons.front().range << std::endl;
		}
		return true;
	}
	else
	{
		if (blinkerBot.Observation()->GetGameLoop() % 30 == 0)
		{
		//	std::cerr << UnitTypeToName(attacker->unit_type) << " range " << blinkerBot.Observation()->GetUnitTypeData()[attacker->unit_type].weapons.front().range << std::endl;
		}
		return false;
	}
}

/*
returns true if the next shot from the enemy will kill our unit's shields
*/
bool ArmyManager::shieldsCritical(const Unit *unit, const Unit *attacker)
{
	if (!unit || !attacker || blinkerBot.Observation()->GetUnitTypeData()[attacker->unit_type].weapons.size() == 0)
	{
		return false;
	}
	//calculate the next attack damage as number of hits x damage per hit (doesn't factor in attack bonuses or upgrades)
	float nextAttackDamage = blinkerBot.Observation()->GetUnitTypeData()[attacker->unit_type].weapons.front().attacks * 
		blinkerBot.Observation()->GetUnitTypeData()[attacker->unit_type].weapons.front().damage_;
	//if the next attack is more than our shields, return true
	if (nextAttackDamage >= unit->shield)
	{
		return true;
	}
	else
	{
		return false;
	}
}

/*
various debug code
*/
void ArmyManager::printDebug()
{
	std::ostringstream us;
	us << "our army: " << currentArmyValue << std::endl;
	std::ostringstream them;
	them << "their army: " << currentEnemyArmyValue << std::endl;
	blinkerBot.Debug()->DebugTextOut(us.str());
	blinkerBot.Debug()->DebugTextOut(them.str());
	blinkerBot.Debug()->SendDebug();
}

/*
upon seeing an enemy structure, add it to the set for attacking purposes
*/
void ArmyManager::addEnemyStructure(const Unit *structure)
{
	if (UnitData::isStructure(structure))
	{
		enemyStructures.insert(structure);
	}
}

/*
when we kill an enemy structure, remove it from the set
*/
void ArmyManager::removeEnemyStructure(const Unit *structure)
{
	for (std::set<const Unit *>::iterator enemyStructure = enemyStructures.begin(); enemyStructure != enemyStructures.end();)
	{
		if ((*enemyStructure) == structure)
		{
			enemyStructures.erase(*enemyStructure++);
		}
		else
		{
			++enemyStructure;
		}
	}
}

/*
returns true if the enemy has either cloaked units, or structures which produce cloaked units
*/
bool ArmyManager::detectionRequired()
{
	bool canCloak = false;
	//check if any enemy units we know about can potentially cloak
	for (auto unit : enemyArmy)
	{
		if (UnitData::canCloak(unit))
		{
			canCloak = true;
		}
	}
	//also check the enemy structures we know about to see if anything cloaked might be in production
	for (auto structure : enemyStructures)
	{
		if ((structure->unit_type == UNIT_TYPEID::PROTOSS_DARKSHRINE) ||
			(structure->unit_type == UNIT_TYPEID::TERRAN_GHOSTACADEMY) ||
			(structure->unit_type == UNIT_TYPEID::TERRAN_STARPORTTECHLAB) ||
			(structure->unit_type == UNIT_TYPEID::ZERG_INFESTATIONPIT) ||
			(structure->unit_type == UNIT_TYPEID::ZERG_LURKERDENMP))
		{
			canCloak = true;
		}
	}

	return canCloak;
}

/*
calculates a Point for a unit to retreat to (based on the closest enemy)
*/
Point2D ArmyManager::getRetreatPoint(const Unit *unit)
{
	const int RETREATDIST = 5;
	Point2D retreatPoint = unit->pos;
	const Unit *closestEnemy = getClosestEnemy(unit);

	//if enemies are closer to our rally point than us, then let's run away from the enemy rather than to the rally point
	if (closestEnemy && Distance2D(closestEnemy->pos, rallyPoint) < Distance2D(unit->pos, rallyPoint) ||
		Distance2D(unit->pos, rallyPoint) < RETREATDIST)
	{
		if (closestEnemy->pos.x > unit->pos.x)
		{
			retreatPoint = Point2D(unit->pos.x - RETREATDIST, unit->pos.y);
		}
		else if (closestEnemy->pos.x < unit->pos.x)
		{
			retreatPoint = Point2D(unit->pos.x + RETREATDIST, unit->pos.y);
		}
		if (closestEnemy->pos.y > unit->pos.y)
		{
			retreatPoint = Point2D(unit->pos.x, unit->pos.y - RETREATDIST);
		}
		else if (closestEnemy->pos.y < unit->pos.y)
		{
			retreatPoint = Point2D(unit->pos.x, unit->pos.y + RETREATDIST);
		}
	}
	//otherwise let's just retreat back to our rally point
	else
	{
		retreatPoint = rallyPoint;
	}

	return retreatPoint;
}

/*
finds the closest townhall to a given unit
*/
const Unit *ArmyManager::getClosestBase(const Unit *unit)
{
	const Unit *closestBase = nullptr;
	for (auto structure : blinkerBot.Observation()->GetUnits())
	{
		if (UnitData::isOurs(structure) && UnitData::isTownHall(structure))
		{
			if (!closestBase || Distance2D(structure->pos, unit->pos) < Distance2D(closestBase->pos, unit->pos))
			{
				closestBase = structure;
			}
		}
	}
	return closestBase;
}

/*
finds the closest townhall to a given point
*/
const Unit *ArmyManager::getClosestBase(Point2D point)
{
	const Unit *closestBase = nullptr;
	for (auto structure : blinkerBot.Observation()->GetUnits())
	{
		if (UnitData::isOurs(structure) && UnitData::isTownHall(structure))
		{
			if (!closestBase || Distance2D(structure->pos, point) < Distance2D(closestBase->pos, point))
			{
				closestBase = structure;
			}
		}
	}
	return closestBase;
}

/*
let's us know if some key upgrades are complete
*/
void ArmyManager::onUpgradeComplete(UpgradeID upgrade)
{
	if (upgrade == UPGRADE_ID::WARPGATERESEARCH)
	{
		warpgateTech = true;
	}
	else if (upgrade == UPGRADE_ID::BLINKTECH)
	{
		blinkTech = true;
	}
	else if (upgrade == UPGRADE_ID::EXTENDEDTHERMALLANCE)
	{
		extendedThermalLanceTech = true;
	}
}

/*
returns true if the enemy army is much larger than ours
*/
bool ArmyManager::behind()
{
	if (blinkTech && calculateSupply(army) * 2 < calculateSupply(enemyArmy))
	{
		return true;
	}
	else
	{
		return false;
	}
}

/*
tells any dark templars we have to attack the enemy
*/
void ArmyManager::darkTemplarHarass()
{
	for (auto dt : darkTemplars)
	{
		//const Unit *target = getClosestEnemyBase(dt);
		const Unit *target = getClosestEnemyBaseWithoutDetection(dt);

		if (dt->cloak != Unit::CloakState::Cloaked && (dt->orders.empty() || dt->orders.front().ability_id != ABILITY_ID::MOVE))
		{
			blinkerBot.Actions()->UnitCommand(dt, ABILITY_ID::MOVE, rallyPoint);
		}
		else if (target && (dt->orders.empty() || dt->orders.front().ability_id != ABILITY_ID::ATTACK))
		{
			blinkerBot.Actions()->UnitCommand(dt, ABILITY_ID::ATTACK, target->pos);
		}
	}
}

/*
finds idle observers and makes them follow one of our units
*/
void ArmyManager::moveObservers()
{
	for (auto observer : observers)
	{
		if (observer->orders.empty() && !army.empty())
		{
			const Unit *unitToFollow = GetRandomEntry(army).unit;
			blinkerBot.Actions()->UnitCommand(observer, ABILITY_ID::SMART, unitToFollow);
		}
	}
}

/*
returns a value representing the threat level posed by static defence within a local radius. 
The threat level is compared against supply counts.
*/
float ArmyManager::calculateEnemyStaticDefenceInRadius(Point2D centre)
{
	float total = 0;
	for (auto structure : enemyStructures)
	{
		if (Distance2D(structure->pos, centre) < LOCALRADIUS && structure->build_progress == 1.0)
		{
			if (structure->unit_type == UNIT_TYPEID::TERRAN_BUNKER ||
				structure->unit_type == UNIT_TYPEID::PROTOSS_PHOTONCANNON ||
				structure->unit_type == UNIT_TYPEID::ZERG_SPINECRAWLER)
			{
				total += 6;
			}
			else if (structure->unit_type == UNIT_TYPEID::TERRAN_PLANETARYFORTRESS)
			{
				total += 20;
			}
		}
	}
	return total;
}

void ArmyManager::updateArmyValues()
{
	currentArmyValue = calculateSupply(army);
	currentEnemyArmyValue = calculateSupply(enemyArmy) + calculateEnemyStaticDefence();
}

/*
returns true if the enemy appears to be doing a rush
*/
bool ArmyManager::rushDetected()
{
	if (!enemyStructures.empty() && blinkerBot.Observation()->GetGameLoop() < 6000)
	{
		//if the enemy is terran or protoss, we can use building position and number to determine their strategy
		if (enemyRace != Race::Zerg)
		{
			//if our opponent chose random, we can update its race once we've scouted its base
			if (enemyRace == Race::Random)
			{
				if (!enemyStructures.empty())
				{
					enemyRace = blinkerBot.Observation()->GetUnitTypeData()[(*enemyStructures.begin())->unit_type].race;
					return false;
				}
				else if (!enemyArmy.empty())
				{
					enemyRace = blinkerBot.Observation()->GetUnitTypeData()[(*enemyArmy.begin())->unit_type].race;
					return false;
				}
			}

			//this check should only be started after the time that we expect them to have built something
			if (blinkerBot.Observation()->GetGameLoop() > 1200)
			{
				//let's count what we have
				int ourProductionFacilities = 0;
				for (auto structure : structures)
				{
					if (structure->unit_type == UNIT_TYPEID::PROTOSS_GATEWAY)
					{
						ourProductionFacilities++;
					}
				}

				//let's count what we can see
				int productionFacilities = 0;
				int bases = 0;
				int gases = 0;

				for (auto structure : enemyStructures)
				{
					if (structure->unit_type == UNIT_TYPEID::TERRAN_BARRACKS ||
						structure->unit_type == UNIT_TYPEID::PROTOSS_GATEWAY)
					{
						productionFacilities++;
					}
					else if (structure->unit_type == UNIT_TYPEID::TERRAN_COMMANDCENTER ||
						structure->unit_type == UNIT_TYPEID::PROTOSS_NEXUS)
					{
						bases++;
					}
					else if (structure->unit_type == UNIT_TYPEID::TERRAN_REFINERY ||
						structure->unit_type == UNIT_TYPEID::PROTOSS_ASSIMILATOR)
					{
						gases++;
					}
					//if they have structures close to our base, then they're proxying us
					if (Distance2D(structure->pos, blinkerBot.Observation()->GetStartLocation()) < 35)
					{
						//std::cerr << "proxy scouted" << std::endl;
						beingRushed = true;
						proxy = true;
						return true;
					}
				}

				if (ourProductionFacilities > 1)
				{
					//if our enemy is going mass barracks or mass gateway, let's play defensive
					if (bases < 2 && gases == 0 &&
						((enemyRace == Race::Protoss && productionFacilities > 3) ||
						(enemyRace == Race::Terran && productionFacilities > 2)))
					{
						//std::cerr << "possible 1 base aggression scouted" << std::endl;
						beingRushed = true;
						return true;
					}
					//if they still don't have anything in their main, they must be proxying us
					else if (productionFacilities == 0)
					{
						//std::cerr << "can't see anything in the main, reacting to proxy" << std::endl;
						beingRushed = true;
						proxy = true;
						return true;
					}
				}
			}
			return false;
		}
		//for zergs
		else
		{
			//if we see a bunch of units early on
			if (currentEnemyArmyValue >= 8 && currentEnemyArmyValue > currentArmyValue * 1.5)
			{
				return true;
			}
			else if (blinkerBot.Observation()->GetGameLoop() > 2500)
			{
				int hatcheries = 0;
				for (auto structure : enemyStructures)
				{
					if (structure->unit_type == UNIT_TYPEID::ZERG_HATCHERY)
					{
						hatcheries++;
					}
				}
				if (hatcheries < 2)
				{
					return true;
				}
			}

			beingRushed = false;
			return false;
		}
	}
	else
	{
		beingRushed = false;
		return false;
	}
}

/*
uses a timer to check if our enemy has zergling speed or not
*/
void ArmyManager::checkForZerglingSpeed()
{
	if (enemyRace == Race::Zerg && !zerglingSpeed)
	{
		//if we haven't got a zergling, let's find one
		if (!zerglingTimer.zergling)
		{
			//try to find a new zergling
			const Unit *zergling = nullptr;;
			for (auto enemy : enemyArmy)
			{
				if (enemy->unit_type == UNIT_TYPEID::ZERG_ZERGLING && enemy->last_seen_game_loop == blinkerBot.Observation()->GetGameLoop())
				{
					zergling = enemy;
				}
			}
			//when we find a new zergling, store its information
			if (zergling)
			{
				zerglingTimer = ZerglingTimer(zergling,
					blinkerBot.Observation()->GetGameLoop() / 22.4f,
					(blinkerBot.Observation()->GetGameLoop() / 22.4f) + 1.0f,
					zergling->pos,
					blinkerBot.Observation()->HasCreep(zergling->pos));
			}
		}
		//if we lose sight of the zergling then get rid of it
		else if (zerglingTimer.zergling->last_seen_game_loop != blinkerBot.Observation()->GetGameLoop())
		{
			zerglingTimer = ZerglingTimer(nullptr, 0, 0, Point2D(0, 0), false);
		}
		//if we're already timing a zergling, let's check the clock
		else if(zerglingTimer.startTime != 0 && blinkerBot.Observation()->GetGameLoop() / 22.4 >= zerglingTimer.endTime)
		{
			float timeTaken = (blinkerBot.Observation()->GetGameLoop() / 22.4) - zerglingTimer.startTime;
			//for zerglings on creep
			if (zerglingTimer.onCreep && blinkerBot.Observation()->HasCreep(zerglingTimer.zergling->pos))
			{
				//if it has moved further than a zergling's speed on creep
				if (zerglingTimer.startPosition.x != 0 && zerglingTimer.startPosition.y != 0 &&
					Distance2D(zerglingTimer.zergling->pos, zerglingTimer.startPosition) > 6)//3.83903f * timeTaken)
				{
					/*
					std::cerr << "time: " << (blinkerBot.Observation()->GetGameLoop() / 22.4) - zerglingTimer.startTime << std::endl;
					std::cerr << "on creep dist " << Distance2D(zerglingTimer.zergling->pos, zerglingTimer.startPosition) << std::endl;
					*/
					std::cerr << "enemy has ling speed" << std::endl;
					zerglingSpeed = true;
				}
			}
			//off creep
			else if (!zerglingTimer.onCreep && !blinkerBot.Observation()->HasCreep(zerglingTimer.zergling->pos))
			{
				//if it has moved further than a zergling's base speed, it has the speed upgrade
				if (zerglingTimer.startPosition.x != 0 && zerglingTimer.startPosition.y != 0 && 
					Distance2D(zerglingTimer.zergling->pos, zerglingTimer.startPosition) >  5)//2.9531f * timeTaken)
				{
					/*
					std::cerr << "time: " << (blinkerBot.Observation()->GetGameLoop() / 22.4) - zerglingTimer.startTime << std::endl;
					std::cerr << "off creep dist " << Distance2D(zerglingTimer.zergling->pos, zerglingTimer.startPosition) << std::endl;
					*/
					zerglingSpeed = true;
				}
			}

			//reset the values
			zerglingTimer.startTime = blinkerBot.Observation()->GetGameLoop() / 22.4f;
			zerglingTimer.endTime = (blinkerBot.Observation()->GetGameLoop() / 22.4f) + 1.0f;
			zerglingTimer.startPosition = zerglingTimer.zergling->pos;
			zerglingTimer.onCreep = blinkerBot.Observation()->HasCreep(zerglingTimer.zergling->pos);
		}
	}
}

/*
finds appropriate targets and issues commands to cast psistorm
*/
void ArmyManager::psistorm()
{
	std::set<const Unit *> enemiesInRange;
	float stormRange = blinkerBot.Observation()->GetAbilityData()[AbilityID(ABILITY_ID::EFFECT_PSISTORM)].cast_range;
	float stormRadius = 1.5f;
	float energyCost = 75.0f;
	for (auto ht : highTemplars)
	{
		if (ht->energy >= energyCost)
		{
			//find local enemies
			enemiesInRange = getEnemiesInRange(ht, ABILITY_ID::EFFECT_PSISTORM);
			//determine the best place to put the storm
			Point2D target = getAOETarget(enemiesInRange, stormRadius);
			if (Distance2D(target, ht->pos) <= stormRange && 
				target.x != 0 && target.y != 0 && 
				(calculateSupplyAndWorkers(enemiesInRange) > 4 || ht->shield == 0))
			{
				blinkerBot.Actions()->UnitCommand(ht, ABILITY_ID::EFFECT_PSISTORM, target);
				return;
			}
		}
	}
}

/*
returns a set of units that are in a given range of a unit casting a given spell
*/
std::set<const Unit *> ArmyManager::getEnemiesInRange(const Unit *unit, AbilityID spell)
{
	float castRange = blinkerBot.Observation()->GetAbilityData()[spell].cast_range;
	std::set<const Unit *> enemiesInRange;
	for (auto enemy : enemyArmy)
	{
		if (enemy->last_seen_game_loop == blinkerBot.Observation()->GetGameLoop() &&
			Distance2D(enemy->pos, unit->pos) <= castRange &&
			enemy->unit_type != UNIT_TYPEID::ZERG_EGG &&
			enemy->unit_type != UNIT_TYPEID::ZERG_LARVA &&
			enemy->unit_type != UNIT_TYPEID::ZERG_CREEPTUMORBURROWED &&
			!UnitData::isStructure(enemy) &&
			UnitData::canTarget(unit, enemy))
		{
			enemiesInRange.insert(enemy);
		}
	}
	return enemiesInRange;
}


/*
returns a Point2D representing the optimal target (centred on a unit) for an AOE spell with a given radius
*/
Point2D ArmyManager::getAOETarget(std::set<const Unit *> unitsInRange, float radius)
{
	//don't waste time calculating if there aren't enough units
	if (unitsInRange.empty())
	{
		return Point2D(0, 0);
	}
	else if (unitsInRange.size() == 1)
	{
		return (*unitsInRange.begin())->pos;
	}

	const Unit *bestTarget = nullptr;
	int highestHitCount = 0;

	for (auto unit : unitsInRange)
	{
		//ignore units already under storm
		if (!isUnderPsistorm(unit->pos))
		{
			//check the number of other units within the attack radius of each unit
			int hitCount = 0;
			for (auto nearby : unitsInRange)
			{
				//count any unit that's in the radius but not under a storm already
				if (Distance2D(unit->pos, nearby->pos) <= radius && !isUnderPsistorm(nearby->pos))
				{
					hitCount++;
				}
			}
			if (hitCount > highestHitCount)
			{
				bestTarget = unit;
				highestHitCount = hitCount;
			}
		}
	}

	if (bestTarget)
	{
		return bestTarget->pos;
	}
	else
	{
		return Point2D(0, 0);
	}
}

/*
calculate the supply of a set of units including workers
*/
float ArmyManager::calculateSupplyAndWorkers(std::set<const Unit *> army)
{
	float total = 0;
	for (auto unit : army)
	{
		total += blinkerBot.Observation()->GetUnitTypeData()[unit->unit_type].food_required;
	}
	return total;
}

/*
returns true if the given point is currently under a psionic storm
*/
bool ArmyManager::isUnderPsistorm(Point2D target)
{
	for (auto effect : blinkerBot.Observation()->GetEffects())
	{
		if (effect.effect_id == AbilityID(ABILITY_ID::EFFECT_PSISTORM))
		{
			//std::cerr << "found a psi storm" << std::endl;
			for (auto position : effect.positions)
			{
				if (position.x == target.x && position.y == target.y)
				{
					return true;
				}
			}
		}
	}
	return false;
}

/*
checks if a unit is under the effect of a hostile AOE spell. Issues a move command and returns true, or false otherwise
*/
bool ArmyManager::escapeAOE(ArmyUnit armyUnit)
{
	if (isUnderHostileSpell(armyUnit.unit->pos))
	{
		if (armyUnit.unit->unit_type == UNIT_TYPEID::PROTOSS_STALKER && blinkTech)
		{
			blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::EFFECT_BLINK, getRetreatPoint(armyUnit.unit));
		}
		else
		{
			blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::MOVE, getRetreatPoint(armyUnit.unit));
		}
		return true;
	}
	else
	{
		return false;
	}
}

/*
finds an appropriate target and casts feedback if possible
*/
void ArmyManager::feedback()
{
	float castRange = blinkerBot.Observation()->GetAbilityData()[AbilityID(ABILITY_ID::EFFECT_FEEDBACK)].cast_range;
	float energyCost = 50.0f;

	for (auto ht : highTemplars)
	{
		if (ht->energy >= energyCost)
		{
			for (auto enemy : enemyArmy)
			{
				if (UnitData::isCaster(enemy->unit_type) && Distance2D(ht->pos, enemy->pos) <= castRange)
				{
					blinkerBot.Actions()->UnitCommand(ht, ABILITY_ID::EFFECT_FEEDBACK, enemy);
					return;
				}
			}
		}
	}
}

/*
returns true if the given point is under a potentially hostile spell
*/
bool ArmyManager::isUnderHostileSpell(Point2D target)
{
	for (auto effect : blinkerBot.Observation()->GetEffects())
	{
		EffectData effectData = blinkerBot.Observation()->GetEffectData()[effect.effect_id];
		if (effectData.effect_id == 1 || //psistorm
			effectData.effect_id == 3 || //temporal field (growing)
			effectData.effect_id == 4 || //temporal field
			effectData.effect_id == 7 || //nuke dot
			effectData.effect_id == 10 || //blinding cloud
			effectData.effect_id == 11 //corrosive bile
			)
		{
			for (auto position : effect.positions)
			{
				if (Distance2D(position, target) <= effectData.radius || 
					(effectData.effect_id == 7 && Distance2D(position, target) <= 8.0f)) //nuke radius
				{
					return true;
				}
			}
		}
	}
	return false;
}

/*
commands nearby stalkers to blink towards an enemy target in some situations
*/
bool ArmyManager::aggressiveBlink(Point2D target)
{
	if (army.empty())
	{
		return false;
	}

	//find a target to blink on top of
	const Unit *blinkTarget = getClosestEnemyFlyer(target);
	if (!blinkTarget)
	{
		//blinkTarget = getClosestEnemy(target);
	}

	//we only want to blink in if our army is significantly larger
	if (blinkTarget && calculateSupplyInRadius(blinkTarget->pos, enemyArmy) * 1.5 < calculateSupplyInRadius(blinkTarget->pos, army))
	{
		for (auto armyUnit : army)
		{
			if (armyUnit.unit->unit_type == UNIT_TYPEID::PROTOSS_STALKER && 
				Distance2D(armyUnit.unit->pos, target) < LOCALRADIUS && Distance2D(armyUnit.unit->pos, target) > 3)
			{
				blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::EFFECT_BLINK, blinkTarget->pos);
			}
		}
		return true;
	}
	else
	{
		return false;
	}

}

/*
returns true if our army includes a given unit type
*/
bool ArmyManager::includes(UnitTypeID unitType)
{
	for (auto armyUnit : army)
	{
		if (armyUnit.unit->unit_type == unitType)
		{
			return true;
		}
	}
	return false;
}

/*
returns the closest visible enemy flying unit, or nullptr if none are found
*/
const Unit *ArmyManager::getClosestEnemyFlyer(Point2D point)
{
	const Unit *closestFlyer = nullptr;
	for (auto enemy : enemyArmy)
	{
		if (enemy->is_flying && enemy->last_seen_game_loop == blinkerBot.Observation()->GetGameLoop())
		{
			if (!closestFlyer)
			{
				closestFlyer = enemy;
			}
			else if (Distance2D(enemy->pos, point) < Distance2D(closestFlyer->pos, point))
			{
				closestFlyer = enemy;
			}
		}
	}
	return closestFlyer;
}

/*
finds the closest enemy structure that is neither a detector or within (static) detector range of a given unit
*/
const Unit *ArmyManager::getClosestEnemyBaseWithoutDetection(const Unit *unit)
{
	const Unit *closestBase = nullptr;
	for (auto structure : enemyStructures)
	{
		//std::cerr << UnitTypeToName(structure->unit_type) << " has a detect range of " << structure->detect_range << std::endl;

		bool inDetectRange = false;

		//if this structure is a detector let's ignore it
		if (structure->detect_range > 0)
		{
			inDetectRange = true;
		}
		//otherwise let's check if it's within the detector range of another structure
		else
		{
			for (auto nearbyStructure : enemyStructures)
			{
				if (nearbyStructure != structure && Distance2D(structure->pos, nearbyStructure->pos) <= nearbyStructure->detect_range)
				{
					//std::cerr << UnitTypeToName(structure->unit_type) << " is within detect range of " <<
						//UnitTypeToName(nearbyStructure->unit_type) << std::endl;
					inDetectRange = true;
				}
			}
		}
		//if it's not in detector range then let's compare it to the current closest
		if (!inDetectRange)
		{
			//if we haven't assigned the closest yet
			if (!closestBase)
			{
				closestBase = structure;
			}
			//if it's closer than the previous closest
			else if (Distance2D(structure->pos, unit->pos) < Distance2D(closestBase->pos, unit->pos))
			{
				closestBase = structure;
			}
		}
	}
	return closestBase;
}

/*
activates prismatic alignment when voidrays come into range of an armoured target
*/
void ArmyManager::activatePrismaticAlignment(const Unit *voidray)
{
	if (voidray->unit_type != UNIT_TYPEID::PROTOSS_VOIDRAY)
	{
		return;
	}

	const Unit *target = getClosestEnemy(voidray);
	float attackRange = (*blinkerBot.Observation()->GetUnitTypeData()[voidray->unit_type].weapons.begin()).range;
	bool armoredTarget = false;

	if (target)
	{
		//check that our target is armoured
		for (auto attribute : blinkerBot.Observation()->GetUnitTypeData()[target->unit_type].attributes)
		{
			if (attribute == Attribute::Armored)
			{
				armoredTarget = true;
			}
		}

		//if the target is armoured and in range, activate prismatic alignment
		if (armoredTarget && Distance2D(target->pos, voidray->pos) <= attackRange)
		{
			blinkerBot.Actions()->UnitCommand(voidray, ABILITY_ID::EFFECT_VOIDRAYPRISMATICALIGNMENT);
		}
	}
}

/*
returns true for zerg opponents that have over 50% zergling based armies (unless they also have mutas).
This is so Production Manager can change to a more zealot-heavy army (obviously we want to keep making stalkers vs mutas).
*/
bool ArmyManager::massLings()
{
	if (enemyRace == Race::Zerg && 
		currentEnemyArmyValue >= 10 && 
		currentEnemyArmyValue / 2 < totalZerglingSupply &&
		!enemyHas(UNIT_TYPEID::ZERG_MUTALISK))
	{
 		return true;
	}
	
	return false;
}

/*
returns true if the enemy army contains the given unit type
*/
bool ArmyManager::enemyHas(UnitTypeID unitType)
{
	for (auto unit : enemyArmy)
	{
		if (unit->unit_type == unitType)
		{
			return true;
		}
	}
	return false;
}

/*
selects targets and issues commands to cast forcefields
*/
void ArmyManager::forcefield()
{
	if (enemyArmy.empty() || sentries.empty())
	{
		return;
	}

	float castRange = blinkerBot.Observation()->GetAbilityData()[AbilityID(ABILITY_ID::EFFECT_FORCEFIELD)].cast_range;
	float energyCost = 50;
	float radius = 2;

	//find a sentry with enough energy
	const Unit *caster = nullptr;
	std::set<const Unit *>::iterator sentry = sentries.begin();
	while (sentry != sentries.end() && !caster)
	{
		if ((*sentry)->energy >= energyCost)
		{
			caster = *sentry;
		}
		sentry++;
	}

	if (caster)
	{
		Point2D target = getDefensiveForcefieldTarget(caster);
		if (target.x != 0 && target.y != 0)
		{
			blinkerBot.Actions()->UnitCommand(caster, ABILITY_ID::EFFECT_FORCEFIELD, target);
		}
		else
		{

		}
		//blinkerBot.Actions()->UnitCommand(sentry, ABILITY_ID::EFFECT_FORCEFIELD, getAOETarget(enemiesInRange, radius));
	}
}

/*
places a forcefield in front of enemy units to push them away
*/
Point2D ArmyManager::getDefensiveForcefieldTarget(const Unit *sentry)
{
	float castRange = blinkerBot.Observation()->GetAbilityData()[AbilityID(ABILITY_ID::EFFECT_FORCEFIELD)].cast_range;
	float radius = 2;
	std::set<const Unit *> localEnemies = getEnemiesInRange(sentry, ABILITY_ID::EFFECT_FORCEFIELD);
	std::set<const Unit *> localObjects;
	const Unit *closestEnemy = nullptr;
	
	//if we don't have any enemies, return 0, 0 to signal that no FF should be placed
	if (localEnemies.empty())
	{
		return Point2D(0, 0);
	}

	//find local blockers (rocks, buildings, or other forcefields)
	for (auto unit : blinkerBot.Observation()->GetUnits())
	{
		if ((UnitData::isStructure(unit) || 
			UnitData::isNeutralRock(unit) || 
			unit->unit_type == UNIT_TYPEID::NEUTRAL_FORCEFIELD) &&
			Distance2D(unit->pos, sentry->pos) <= castRange)
		{
			localObjects.insert(unit);
		}
	}

	const Unit *bestTarget = nullptr;
	for (auto enemy : localEnemies)
	{
		//filter flying and massive units
		if (!enemy->is_flying && !hasAttribute(enemy->unit_type, Attribute::Massive))
		{
			//make sure that there is not already a blocker here
			bool blocked = false;
			for (auto object : localObjects)
			{
				if (Distance2D(enemy->pos, object->pos) <= object->radius)
				{
					blocked = true;
				}
			}
			//count the number of nearby enemies to this target
			if (!blocked)
			{
				if (!bestTarget)
				{
					bestTarget = enemy;
				}
				else if (Distance2D(enemy->pos, sentry->pos) < Distance2D(bestTarget->pos, sentry->pos))
				{
					bestTarget = enemy;
				}
			}
		}
	}

	if (bestTarget)
	{
		//if we are planning to put a forcefield near a ramp, let's try to centre it on the ramp
		Point2D ramp = findNearbyRamp(sentry);
		if (ramp.x != 0 && ramp.y != 0 && Distance2D(ramp, bestTarget->pos) < 6)
		{
			return ramp;
		}
		else
		{
			return bestTarget->pos;
		}
	}
	else
	{
		return Point2D(0, 0);
	}
}

/*
returns true if the unit type has the specified attribute (e.g. colossus returns true for Massive)
*/
bool ArmyManager::hasAttribute(UnitTypeID unitType, Attribute attribute)
{
	for (auto att : blinkerBot.Observation()->GetUnitTypeData()[unitType].attributes)
	{
		if (att == attribute)
		{
			return true;
		}
	}
	return false;
}

/*
returns the location of a nearby ramp; or 0, 0 if no ramp can be found;
*/
Point2D ArmyManager::findNearbyRamp(const Unit *unit)
{
	std::vector<Point2D> grid = calculateGrid(unit->pos, LOCALRADIUS);
	std::vector<Point2D> rampPoints;
	Point2D target = Point2D(0, 0);

	//check each point on the grid. If it's pathable but not placable then we'll count it as a ramp
	for (auto point : grid)
	{
		if (blinkerBot.Observation()->IsPathable(point) && !blinkerBot.Observation()->IsPlacable(point))
		{
			rampPoints.push_back(point);
		}
	}

	//let's calculate an average point and remove anomalies
	float averageX = 0;
	float averageY = 0;
	for (auto point : rampPoints)
	{
		averageX += point.x;
		averageY += point.y;
	}
	averageX = averageX / rampPoints.size();
	averageY = averageY / rampPoints.size();
	for (std::vector<Point2D>::iterator point = rampPoints.begin(); point != rampPoints.end();)
	{
		if (Distance2D(*point, Point2D(averageX, averageY)) > 8)
		{
			point = rampPoints.erase(point);
		}
		else
		{
			++point;
		}
	}

	//recalculate the average with no anomalies
	averageX = 0;
	averageY = 0;
	for (auto point : rampPoints)
	{
		averageX += point.x;
		averageY += point.y;
	}
	averageX = averageX / rampPoints.size();
	averageY = averageY / rampPoints.size();

	//make sure our calculated point is on a ramp
	if (blinkerBot.Observation()->IsPathable(Point2D(averageX, averageY)) && 
		!blinkerBot.Observation()->IsPlacable(Point2D(averageX, averageY)))
	{
		target = Point2D(averageX, averageY);
	}

	return target;
}

/*
returns a grid of Point2Ds with int values with a given centre and extending in positive and negative directions to a given limit.
*/
std::vector<Point2D> ArmyManager::calculateGrid(Point2D centre, int size)
{
	std::vector<Point2D> grid;

	//first we set the minimum and maximum values for the search area
	float minX = centre.x - size;
	float minY = centre.y - size;
	float maxX = centre.x + size;
	float maxY = centre.y + size;

	if (minX < 0)
	{
		minX = 0;
	}
	if (minY < 0)
	{
		minY = 0;
	}
	if (maxX > blinkerBot.Observation()->GetGameInfo().width)
	{
		maxX = float(blinkerBot.Observation()->GetGameInfo().width);
	}
	if (maxY > blinkerBot.Observation()->GetGameInfo().height)
	{
		maxY = float(blinkerBot.Observation()->GetGameInfo().height);
	}

	//make a vector of points within range
	for (float x = minX; x <= maxX; x++)
	{
		for (float y = minY; y <= maxY; y++)
		{
			grid.push_back(Point2D(x, y));
		}
	}
	return grid;
}

bool ArmyManager::lingSpeed()
{
	return zerglingSpeed;
}

/*
adds a structure to the set of our structures
*/
void ArmyManager::addStructure(const Unit *structure)
{
	if (UnitData::isOurs(structure) && UnitData::isStructure(structure))
	{
		structures.insert(structure);
	}
}

/*
removes a structure from the set of our structures
*/
void ArmyManager::removeStructure(const Unit *structure)
{
	structures.erase(structure);
}

/*
destroys the target building (used to break accidental wall-ins)
*/
void ArmyManager::breakWall(const Unit *blocker)
{
	if (blocker)
	{
		demolitionDuty = true;
		std::cerr << "attacking the blocker" << std::endl;
		for (auto armyUnit : army)
		{
			if (armyUnit.unit->orders.empty() || armyUnit.unit->orders.front().ability_id != ABILITY_ID::ATTACK)
			{
				blinkerBot.Actions()->UnitCommand(armyUnit.unit, ABILITY_ID::ATTACK, blocker);
			}
		}
	}
	else
	{
		demolitionDuty = false;
	}
}

/*
if we have any enemy structures in our main, attack them
*/
bool ArmyManager::checkForProxies()
{
	//find our main nexus
	const Unit *main = nullptr;
	for (auto base : bases)
	{
		if (Distance2D(base->pos, blinkerBot.Observation()->GetStartLocation()) < 5)
		{
			main = base;
		}
	}
	if (main)
	{
		for (auto structure : enemyStructures)
		{
			//check enemy structures in range
			if (Distance2D(blinkerBot.Observation()->GetStartLocation(), structure->pos) <= LOCALRADIUS * 2)
			{
				//compare their z value to make sure they're in the main and not proxied in the natural
				if (structure->pos.z == main->pos.z || 
				   (structure->pos.z > main->pos.z && structure->pos.z - main->pos.z < 0.5) ||
				   (structure->pos.z < main->pos.z && main->pos.z - structure->pos.z < 0.5))
				{
					return true;
				}
			}
		}
	}
	return false;
}