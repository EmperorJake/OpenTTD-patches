/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file vehiclelist.h Functions and type for generating vehicle lists. */

#ifndef VEHICLELIST_H
#define VEHICLELIST_H

#include "vehicle_type.h"
#include "company_type.h"
#include "tile_type.h"
#include "cargo_type.h"
#include <vector>

/** Vehicle List type flags */
enum VehicleListType {
	VL_STANDARD,
	VL_SHARED_ORDERS,
	VL_STATION_LIST,
	VL_DEPOT_LIST,
	VL_GROUP_LIST,
	VL_SLOT_LIST,
	VL_SINGLE_VEH,
	VLT_END
};

/** The information about a vehicle list. */
struct VehicleListIdentifier {
	VehicleListType type; ///< The type of vehicle list.
	VehicleType vtype;    ///< The vehicle type associated with this list.
	CompanyID company;    ///< The company associated with this list.
	uint32_t index;       ///< A vehicle list type specific index.

	uint32_t Pack() const;
	bool UnpackIfValid(uint32_t data);
	static VehicleListIdentifier UnPack(uint32_t data);

	/**
	 * Create a simple vehicle list.
	 * @param type    List type.
	 * @param vtype   Vehicle type associated with this list.
	 * @param company Company associated with this list.
	 * @param index   Optional type specific index.
	 */
	VehicleListIdentifier(VehicleListType type, VehicleType vtype, CompanyID company, uint index = 0) :
		type(type), vtype(vtype), company(company), index(index) {}

	VehicleListIdentifier() : type(), vtype(), company(), index() {}
};

/** A list of vehicles. */
typedef std::vector<const Vehicle *> VehicleList;

bool VehicleCargoFilter(const Vehicle *v, const CargoID cid);
bool GenerateVehicleSortList(VehicleList *list, const VehicleListIdentifier &identifier, const CargoID cid = CargoFilterCriteria::CF_ANY);
void BuildDepotVehicleList(VehicleType type, TileIndex tile, VehicleList *engine_list, VehicleList *wagon_list, bool individual_wagons = false);
uint GetUnitNumberDigits(VehicleList &vehicles);

#endif /* VEHICLELIST_H */
