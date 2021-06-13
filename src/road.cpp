/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file road.cpp Generic road related functions. */

#include "stdafx.h"
#include <algorithm>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <vector>
#include "rail_map.h"
#include "road_map.h"
#include "water_map.h"
#include "genworld.h"
#include "company_func.h"
#include "company_base.h"
#include "engine_base.h"
#include "date_func.h"
#include "landscape.h"
#include "road.h"
#include "town.h"
#include "pathfinder/npf/aystar.h"
#include "tunnelbridge.h"
#include "road_func.h"
#include "roadveh.h"
#include "map_func.h"
#include "core/backup_type.hpp"
#include "core/random_func.hpp"

#include <numeric>

#include "cheat_func.h"
#include "command_func.h"
#include "safeguards.h"

uint32 _road_layout_change_counter = 0;

/** Whether to build public roads */
enum PublicRoadsConstruction {
	PRC_NONE,         ///< Generate no public roads
	PRC_WITH_CURVES,  ///< Generate roads with lots of curves
	PRC_AVOID_CURVES, ///< Generate roads avoiding curves if possible
};

/**
 * Return if the tile is a valid tile for a crossing.
 *
 * @param tile the current tile
 * @param ax the axis of the road over the rail
 * @return true if it is a valid tile
 */
static bool IsPossibleCrossing(const TileIndex tile, Axis ax)
{
	return (IsTileType(tile, MP_RAILWAY) &&
		GetRailTileType(tile) == RAIL_TILE_NORMAL &&
		GetTrackBits(tile) == (ax == AXIS_X ? TRACK_BIT_Y : TRACK_BIT_X) &&
		GetFoundationSlope(tile) == SLOPE_FLAT);
}

/**
 * Clean up unnecessary RoadBits of a planned tile.
 * @param tile current tile
 * @param org_rb planned RoadBits
 * @return optimised RoadBits
 */
RoadBits CleanUpRoadBits(const TileIndex tile, RoadBits org_rb)
{
	if (!IsValidTile(tile)) return ROAD_NONE;
	for (DiagDirection dir = DIAGDIR_BEGIN; dir < DIAGDIR_END; dir++) {
		TileIndex neighbor_tile = TileAddByDiagDir(tile, dir);

		/* Get the Roadbit pointing to the neighbor_tile */
		const RoadBits target_rb = DiagDirToRoadBits(dir);

		/* If the roadbit is in the current plan */
		if (org_rb & target_rb) {
			bool connective = false;
			const RoadBits mirrored_rb = MirrorRoadBits(target_rb);

			test_tile:
			if (IsValidTile(neighbor_tile)) {
				switch (GetTileType(neighbor_tile)) {
					/* Always connective ones */
					case MP_CLEAR: case MP_TREES:
						connective = true;
						break;

					/* The conditionally connective ones */
					case MP_TUNNELBRIDGE:
					case MP_STATION:
					case MP_ROAD:
						if (IsNormalRoadTile(neighbor_tile)) {
							/* Always connective */
							connective = true;
						} else {
							const RoadBits neighbor_rb = GetAnyRoadBits(neighbor_tile, RTT_ROAD) | GetAnyRoadBits(neighbor_tile, RTT_TRAM);

							/* Accept only connective tiles */
							connective = (neighbor_rb & mirrored_rb) != ROAD_NONE;
						}
						break;

					case MP_RAILWAY: {
						if (IsPossibleCrossing(neighbor_tile, DiagDirToAxis(dir))) {
							/* Check far side of crossing */
							neighbor_tile = TileAddByDiagDir(neighbor_tile, dir);
							goto test_tile;
						}
						break;
					}

					case MP_WATER:
						/* Check for real water tile */
						connective = !IsWater(neighbor_tile);
						break;

					/* The definitely not connective ones */
					default: break;
				}
			}

			/* If the neighbor tile is inconnective, remove the planned road connection to it */
			if (!connective) org_rb ^= target_rb;
		}
	}

	return org_rb;
}

/**
 * Finds out, whether given company has a given RoadType available for construction.
 * @param company ID of company
 * @param roadtypet RoadType to test
 * @return true if company has the requested RoadType available
 */
bool HasRoadTypeAvail(const CompanyID company, RoadType roadtype)
{
	if (company == OWNER_DEITY || company == OWNER_TOWN || _game_mode == GM_EDITOR || _generating_world) {
		return true; // TODO: should there be a proper check?
	} else {
		const Company *c = Company::GetIfValid(company);
		if (c == nullptr) return false;
		return HasBit(c->avail_roadtypes & ~_roadtypes_hidden_mask, roadtype);
	}
}

static RoadTypes GetMaskForRoadTramType(RoadTramType rtt)
{
	return rtt == RTT_TRAM ? _roadtypes_type : ~_roadtypes_type;
}

/**
 * Test if any buildable RoadType is available for a company.
 * @param company the company in question
 * @return true if company has any RoadTypes available
 */
bool HasAnyRoadTypesAvail(CompanyID company, RoadTramType rtt)
{
	return (Company::Get(company)->avail_roadtypes & ~_roadtypes_hidden_mask & GetMaskForRoadTramType(rtt)) != ROADTYPES_NONE;
}

/**
 * Validate functions for rail building.
 * @param roadtype road type to check.
 * @return true if the current company may build the road.
 */
bool ValParamRoadType(RoadType roadtype)
{
	return roadtype != INVALID_ROADTYPE && HasRoadTypeAvail(_current_company, roadtype);
}

/**
 * Add the road types that are to be introduced at the given date.
 * @param rt      Roadtype
 * @param current The currently available roadtypes.
 * @param date    The date for the introduction comparisons.
 * @return The road types that should be available when date
 *         introduced road types are taken into account as well.
 */
RoadTypes AddDateIntroducedRoadTypes(RoadTypes current, Date date)
{
	RoadTypes rts = current;

	for (RoadType rt = ROADTYPE_BEGIN; rt != ROADTYPE_END; rt++) {
		const RoadTypeInfo *rti = GetRoadTypeInfo(rt);
		/* Unused road type. */
		if (rti->label == 0) continue;

		/* Not date introduced. */
		if (!IsInsideMM(rti->introduction_date, 0, MAX_DAY)) continue;

		/* Not yet introduced at this date. */
		if (rti->introduction_date > date) continue;

		/* Have we introduced all required roadtypes? */
		RoadTypes required = rti->introduction_required_roadtypes;
		if ((rts & required) != required) continue;

		rts |= rti->introduces_roadtypes;
	}

	/* When we added roadtypes we need to run this method again; the added
	 * roadtypes might enable more rail types to become introduced. */
	return rts == current ? rts : AddDateIntroducedRoadTypes(rts, date);
}

/**
 * Get the road types the given company can build.
 * @param company the company to get the road types for.
 * @param introduces If true, include road types introduced by other road types
 * @return the road types.
 */
RoadTypes GetCompanyRoadTypes(CompanyID company, bool introduces)
{
	RoadTypes rts = ROADTYPES_NONE;

	for (const Engine *e : Engine::IterateType(VEH_ROAD)) {
		const EngineInfo *ei = &e->info;

		if (HasBit(ei->climates, _settings_game.game_creation.landscape) &&
				(HasBit(e->company_avail, company) || _date >= e->intro_date + DAYS_IN_YEAR)) {
			const RoadVehicleInfo *rvi = &e->u.road;
			assert(rvi->roadtype < ROADTYPE_END);
			if (introduces) {
				rts |= GetRoadTypeInfo(rvi->roadtype)->introduces_roadtypes;
			} else {
				SetBit(rts, rvi->roadtype);
			}
		}
	}

	if (introduces) return AddDateIntroducedRoadTypes(rts, _date);
	return rts;
}

/* ========================================================================= */
/*                                PUBLIC ROADS                               */
/* ========================================================================= */

CommandCost CmdBuildBridge(TileIndex end_tile, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text = nullptr);
CommandCost CmdBuildTunnel(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text = nullptr);
CommandCost CmdBuildRoad(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text = nullptr);

static std::vector<TileIndex> _town_centers;
static std::vector<TileIndex> _towns_visited_along_the_way;
static bool _has_tunnel_in_path;
static RoadType _public_road_type;
static const uint _public_road_hash_size = 8U; ///< The number of bits the hash for river finding should have.

static const int32 BASE_COST = 1;          // Cost for utilizing an existing road, bridge, or tunnel.
static const int32 COST_FOR_NEW_ROAD = 10; // Cost for building a new road.
static const int32 COST_FOR_SLOPE = 5;     // Additional cost if the road heads up or down a slope.

/** AyStar callback for getting the cost of the current node. */
static int32 PublicRoad_CalculateG(AyStar *, AyStarNode *current, OpenListNode *parent)
{
	int32 cost = BASE_COST;

	if (!IsTileType(current->tile, MP_ROAD)) {
		if (!AreTilesAdjacent(parent->path.node.tile, current->tile))
		{
			// We're not adjacent, so we built a tunnel or bridge.
			cost += (DistanceManhattan(parent->path.node.tile, current->tile)) * COST_FOR_NEW_ROAD + 6 * COST_FOR_SLOPE;
		}
		else if (!IsTileFlat(current->tile)) {
			cost += COST_FOR_NEW_ROAD;
			cost += COST_FOR_SLOPE;
		}
		else
		{
			cost += COST_FOR_NEW_ROAD;
		}
	}

	if (_settings_game.game_creation.build_public_roads == PRC_AVOID_CURVES &&
		parent->path.parent != nullptr &&
		DiagdirBetweenTiles(parent->path.parent->node.tile, parent->path.node.tile) != DiagdirBetweenTiles(parent->path.node.tile, current->tile)) {
		cost += 1;
	}

	return cost;
}

/** AyStar callback for getting the estimated cost to the destination. */
static int32 PublicRoad_CalculateH(AyStar *aystar, AyStarNode *current, OpenListNode *parent)
{
	return DistanceManhattan(*static_cast<TileIndex*>(aystar->user_target), current->tile) * BASE_COST;
}

/** Helper function to check if a tile along a certain direction is going up an inclined slope. */
static bool IsUpwardsSlope(TileIndex tile, DiagDirection road_direction)
{
	const auto slope = GetTileSlope(tile);

	if (!IsInclinedSlope(slope)) return false;

	const auto slope_direction = GetInclinedSlopeDirection(slope);

	return road_direction == slope_direction;
}

/** Helper function to check if a tile along a certain direction is going down an inclined slope. */
static bool IsDownwardsSlope(const TileIndex tile, const DiagDirection road_direction)
{
	const auto slope = GetTileSlope(tile);

	if (!IsInclinedSlope(slope)) return false;

	const auto slope_direction = GetInclinedSlopeDirection(slope);

	return road_direction == ReverseDiagDir(slope_direction);
}

static TileIndex BuildTunnel(PathNode *current, TileIndex end_tile = INVALID_TILE, const bool build_tunnel = false)
{
	const TileIndex start_tile = current->node.tile;
	int start_z;
	GetTileSlope(start_tile, &start_z);

	if (start_z == 0) return INVALID_TILE;

	const DiagDirection direction = GetInclinedSlopeDirection(GetTileSlope(start_tile));

	if (!build_tunnel) {
		// We are not building yet, so we still need to find the end_tile.
		const TileIndexDiff delta = TileOffsByDiagDir(direction);
		end_tile = start_tile;
		int end_z;

		for (int tunnel_length = 1;;tunnel_length++) {
			end_tile += delta;

			if (!IsValidTile(end_tile)) return INVALID_TILE;
			if (tunnel_length > _settings_game.construction.max_tunnel_length) return INVALID_TILE;

			GetTileSlope(end_tile, &end_z);

			if (start_z == end_z) break;

			if (!_cheats.crossing_tunnels.value && IsTunnelInWay(end_tile, start_z)) return INVALID_TILE;
		}
		
		// No too long or super-short tunnels and always ending up on a matching upwards slope.
		if (IsSteepSlope(GetTileSlope(end_tile)) || IsHalftileSlope(GetTileSlope(end_tile))) return INVALID_TILE;
		if (GetTileSlope(start_tile) != ComplementSlope(GetTileSlope(end_tile))) return INVALID_TILE;
		if (AreTilesAdjacent(start_tile, end_tile)) return INVALID_TILE;
		if (!IsValidTile(end_tile)) return INVALID_TILE;
		if (!IsTileType(end_tile, MP_CLEAR) && !IsTileType(end_tile, MP_TREES)) return INVALID_TILE;
	}

	assert(!build_tunnel || (IsValidTile(end_tile) && GetTileSlope(start_tile) == ComplementSlope(GetTileSlope(end_tile))));

	Backup cur_company(_current_company, OWNER_DEITY, FILE_LINE);
	const auto build_tunnel_cmd = CmdBuildTunnel(start_tile, build_tunnel ? DC_EXEC : DC_NONE, _public_road_type | (TRANSPORT_ROAD << 8), 0);
	cur_company.Restore();

	assert(!build_tunnel || build_tunnel_cmd.Succeeded());
	assert(!build_tunnel || (IsTileType(start_tile, MP_TUNNELBRIDGE) && IsTileType(end_tile, MP_TUNNELBRIDGE)));

	if (!build_tunnel_cmd.Succeeded()) return INVALID_TILE;

	return end_tile;
}

static TileIndex BuildBridge(PathNode *current, TileIndex end_tile = INVALID_TILE, const bool build_bridge = false)
{
	const TileIndex start_tile = current->node.tile;

	const DiagDirection direction = ReverseDiagDir(GetInclinedSlopeDirection(GetTileSlope(start_tile)));

	if (!build_bridge) {
		// We are not building yet, so we still need to find the end_tile.
		for (TileIndex tile = start_tile + TileOffsByDiagDir(direction);
			IsValidTile(tile) &&
			(GetTunnelBridgeLength(start_tile, tile) <= _settings_game.construction.max_bridge_length) &&
			(GetTileZ(start_tile) < (GetTileZ(tile) + _settings_game.construction.max_bridge_height)) &&
			(GetTileZ(tile) <= GetTileZ(start_tile));
			tile += TileOffsByDiagDir(direction)) {

			auto is_complementary_slope =
				!IsSteepSlope(GetTileSlope(tile)) &&
				!IsHalftileSlope(GetTileSlope(tile)) &&
				GetTileSlope(start_tile) == ComplementSlope(GetTileSlope(tile));

			// No super-short bridges and always ending up on a matching upwards slope.
			if (!AreTilesAdjacent(start_tile, tile) && is_complementary_slope) {
				end_tile = tile;
				break;
			}
		}

		if (!IsValidTile(end_tile)) return INVALID_TILE;
		if (GetTileSlope(start_tile) != ComplementSlope(GetTileSlope(end_tile))) return INVALID_TILE;
		if (!IsTileType(end_tile, MP_CLEAR) && !IsTileType(end_tile, MP_TREES)) return INVALID_TILE;
	}

	assert(!build_bridge || (IsValidTile(end_tile) && GetTileSlope(start_tile) == ComplementSlope(GetTileSlope(end_tile))));

	std::vector<BridgeType> available_bridge_types;

	for (uint i = 0; i < MAX_BRIDGES; ++i) {
		if (CheckBridgeAvailability(i, GetTunnelBridgeLength(start_tile, end_tile)).Succeeded()) {
			available_bridge_types.push_back(i);
		}
	}

	assert(!build_bridge || !available_bridge_types.empty());
	if (available_bridge_types.empty()) return INVALID_TILE;

	const auto bridge_type = available_bridge_types[build_bridge ? RandomRange(uint32(available_bridge_types.size())) : 0];

	Backup cur_company(_current_company, OWNER_DEITY, FILE_LINE);
	const auto build_bridge_cmd = CmdBuildBridge(end_tile, build_bridge ? DC_EXEC : DC_NONE, start_tile, bridge_type | (ROADTYPE_ROAD << 8) | (TRANSPORT_ROAD << 15));
	cur_company.Restore();

	assert(!build_bridge || build_bridge_cmd.Succeeded());
	assert(!build_bridge || (IsTileType(start_tile, MP_TUNNELBRIDGE) && IsTileType(end_tile, MP_TUNNELBRIDGE)));

	if (!build_bridge_cmd.Succeeded()) return INVALID_TILE;

	return end_tile;
}

static TileIndex BuildRiverBridge(PathNode *current, const DiagDirection road_direction, TileIndex end_tile = INVALID_TILE, const bool build_bridge = false)
{
	const TileIndex start_tile = current->node.tile;

	if (!build_bridge) {
		// We are not building yet, so we still need to find the end_tile.
		// We will only build a bridge if we need to cross a river, so first check for that.
		TileIndex tile = start_tile + TileOffsByDiagDir(road_direction);

		if (!IsWaterTile(tile) || !IsRiver(tile)) return INVALID_TILE;

		// Now let's see if we can bridge it. But don't bridge anything more than 4 river tiles. Cities aren't allowed to, so public roads we are not either.
		// Only bridges starting at slopes should be longer ones. The others look like crap when built this way. Players can build them but the map generator
		// should not force that on them. This is just to bridge rivers, not to make long bridges.
		for (;
			IsValidTile(tile) &&
			(GetTunnelBridgeLength(start_tile, tile) <= 5) &&
			(GetTileZ(start_tile) < (GetTileZ(tile) + _settings_game.construction.max_bridge_height)) &&
			(GetTileZ(tile) <= GetTileZ(start_tile));
			tile += TileOffsByDiagDir(road_direction)) {

			if ((IsTileType(tile, MP_CLEAR) || IsTileType(tile, MP_TREES)) &&
				GetTileZ(tile) <= GetTileZ(start_tile) &&
				GetTileSlope(tile) == SLOPE_FLAT) {
				end_tile = tile;
				break;
			}
		}

		if (!IsValidTile(end_tile)) return INVALID_TILE;
		if (!IsTileType(end_tile, MP_CLEAR) && !IsTileType(end_tile, MP_TREES)) return INVALID_TILE;
	}

	assert(!build_bridge || IsValidTile(end_tile));

	std::vector<BridgeType> available_bridge_types;

	for (uint i = 0; i < MAX_BRIDGES; ++i) {
		if (CheckBridgeAvailability(i, GetTunnelBridgeLength(start_tile, end_tile)).Succeeded()) {
			available_bridge_types.push_back(i);
		}
	}

	const auto bridge_type = available_bridge_types[build_bridge ? RandomRange(uint32(available_bridge_types.size())) : 0];

	Backup cur_company(_current_company, OWNER_DEITY, FILE_LINE);
	const auto build_bridge_cmd = CmdBuildBridge(end_tile, build_bridge ? DC_EXEC : DC_NONE, start_tile, bridge_type | (ROADTYPE_ROAD << 8) | (TRANSPORT_ROAD << 15));
	cur_company.Restore();

	assert(!build_bridge || build_bridge_cmd.Succeeded());
	assert(!build_bridge || (IsTileType(start_tile, MP_TUNNELBRIDGE) && IsTileType(end_tile, MP_TUNNELBRIDGE)));

	if (!build_bridge_cmd.Succeeded()) return INVALID_TILE;

	return end_tile;
}

static bool IsValidNeighbourOfPreviousTile(const TileIndex tile, const TileIndex previous_tile)
{
	if (!IsValidTile(tile) || (tile == previous_tile)) return false;

	if (IsTileType(tile, MP_TUNNELBRIDGE))
	{
		if (GetOtherTunnelBridgeEnd(tile) == previous_tile) return true;

		const auto tunnel_direction = GetTunnelBridgeDirection(tile);

		if (previous_tile + TileOffsByDiagDir(tunnel_direction) != tile) return false;
	} else {

		if (!IsTileType(tile, MP_CLEAR) && !IsTileType(tile, MP_TREES) && !IsTileType(tile, MP_ROAD)) return false;

		const auto slope = GetTileSlope(tile);

		// Do not allow foundations. We'll mess things up later.
		const bool has_foundation = GetFoundationSlope(tile) != slope;

		if (has_foundation) return false;

		if (IsInclinedSlope(slope)) {
			const auto slope_direction = GetInclinedSlopeDirection(slope);
			const auto road_direction = DiagdirBetweenTiles(previous_tile, tile);

			if (slope_direction != road_direction && ReverseDiagDir(slope_direction) != road_direction) {
				return false;
			}
		} else if (slope != SLOPE_FLAT) {
			return false;
		}
	}

	return true;
}

/** AyStar callback for getting the neighbouring nodes of the given node. */
static void PublicRoad_GetNeighbours(AyStar *aystar, OpenListNode *current)
{
	const TileIndex tile = current->path.node.tile;

	aystar->num_neighbours = 0;

	// Check if we just went through a tunnel or a bridge.
	if (current->path.parent != nullptr && !AreTilesAdjacent(tile, current->path.parent->node.tile)) {
		const auto previous_tile = current->path.parent->node.tile;

		// We went through a tunnel or bridge, this limits our options to proceed to only forward.
		const auto tunnel_bridge_direction = DiagdirBetweenTiles(previous_tile, tile);

		const TileIndex tunnel_bridge_end = tile + TileOffsByDiagDir(tunnel_bridge_direction);

		if (IsValidNeighbourOfPreviousTile(tunnel_bridge_end, tile)) {
			aystar->neighbours[aystar->num_neighbours].tile = tunnel_bridge_end;
			aystar->neighbours[aystar->num_neighbours].direction = INVALID_TRACKDIR;
			aystar->num_neighbours++;
		}
	} else {
		// Handle all the regular neighbours and existing tunnels/bridges.
		std::vector<TileIndex> potential_neighbours;

		if (IsTileType(tile, MP_TUNNELBRIDGE)) {
			auto neighbour = GetOtherTunnelBridgeEnd(tile);

			aystar->neighbours[aystar->num_neighbours].tile = neighbour;
			aystar->neighbours[aystar->num_neighbours].direction = INVALID_TRACKDIR;
			aystar->num_neighbours++;

			neighbour = tile + TileOffsByDiagDir(ReverseDiagDir(DiagdirBetweenTiles(tile, neighbour)));

			if (IsValidNeighbourOfPreviousTile(neighbour, tile)) {
				aystar->neighbours[aystar->num_neighbours].tile = neighbour;
				aystar->neighbours[aystar->num_neighbours].direction = INVALID_TRACKDIR;
				aystar->num_neighbours++;
			}
		} else {
			for (DiagDirection d = DIAGDIR_BEGIN; d < DIAGDIR_END; d++) {
				const auto neighbour = tile + TileOffsByDiagDir(d);

				if (IsValidNeighbourOfPreviousTile(neighbour, tile)) {
					aystar->neighbours[aystar->num_neighbours].tile = neighbour;
					aystar->neighbours[aystar->num_neighbours].direction = INVALID_TRACKDIR;
					aystar->num_neighbours++;
				}
			}
			
			// Check if we can turn this into a tunnel or a bridge.
			if (current->path.parent != nullptr) {
				const auto road_direction = DiagdirBetweenTiles(current->path.parent->node.tile, tile);

				if (IsUpwardsSlope(tile, road_direction) && !_has_tunnel_in_path) {
					const auto tunnel_end = BuildTunnel(&current->path);

					if (tunnel_end != INVALID_TILE &&
						!IsSteepSlope(GetTileSlope(tunnel_end)) &&
						!IsHalftileSlope(GetTileSlope(tunnel_end)) && 
						(GetTileSlope(tunnel_end) == ComplementSlope(GetTileSlope(current->path.node.tile)))) {
						assert(IsValidDiagDirection(DiagdirBetweenTiles(tile, tunnel_end)));
						aystar->neighbours[aystar->num_neighbours].tile = tunnel_end;
						aystar->neighbours[aystar->num_neighbours].direction = INVALID_TRACKDIR;
						aystar->num_neighbours++;
						_has_tunnel_in_path = true;
					}
				}
				else if (IsDownwardsSlope(tile, road_direction)) {
					const auto bridge_end = BuildBridge(&current->path);

					if (bridge_end != INVALID_TILE &&
						!IsSteepSlope(GetTileSlope(bridge_end)) &&
						!IsHalftileSlope(GetTileSlope(bridge_end)) && 
						(GetTileSlope(bridge_end) == ComplementSlope(GetTileSlope(current->path.node.tile)))) {
						assert(IsValidDiagDirection(DiagdirBetweenTiles(tile, bridge_end)));
						aystar->neighbours[aystar->num_neighbours].tile = bridge_end;
						aystar->neighbours[aystar->num_neighbours].direction = INVALID_TRACKDIR;
						aystar->num_neighbours++;
					}
				}
				else if (GetTileSlope(tile) == SLOPE_FLAT)
				{
					// Check if we could bridge a river from a flat tile. Not looking pretty on the map but you gotta do what you gotta do.
					const auto bridge_end = BuildRiverBridge(&current->path, DiagdirBetweenTiles(current->path.parent->node.tile, tile));
					assert(bridge_end == INVALID_TILE || GetTileSlope(bridge_end) == SLOPE_FLAT);

					if (bridge_end != INVALID_TILE) {
						assert(IsValidDiagDirection(DiagdirBetweenTiles(tile, bridge_end)));
						aystar->neighbours[aystar->num_neighbours].tile = bridge_end;
						aystar->neighbours[aystar->num_neighbours].direction = INVALID_TRACKDIR;
						aystar->num_neighbours++;
					}
				}
			}
		}
	}
}

/** AyStar callback for checking whether we reached our destination. */
static int32 PublicRoad_EndNodeCheck(const AyStar *aystar, const OpenListNode *current)
{
	// Mark towns visited along the way.
	const auto search_result =
		std::find(_town_centers.begin(), _town_centers.end(), current->path.node.tile);

	if (search_result != _town_centers.end()) {
		_towns_visited_along_the_way.push_back(current->path.node.tile);
	}

	return current->path.node.tile == *static_cast<TileIndex*>(aystar->user_target) ? AYSTAR_FOUND_END_NODE : AYSTAR_DONE;
}

/** AyStar callback when an route has been found. */
static void PublicRoad_FoundEndNode(AyStar *aystar, OpenListNode *current)
{
	PathNode* child = nullptr;

	for (PathNode *path = &current->path; path != nullptr; path = path->parent) {
		const TileIndex tile = path->node.tile;

		if (IsTileType(tile, MP_TUNNELBRIDGE)) {
			// Just follow the path; infrastructure is already in place.
			continue;
		}

		if (path->parent == nullptr || AreTilesAdjacent(tile, path->parent->node.tile)) {
			RoadBits road_bits = ROAD_NONE;

			if (child != nullptr) {
				const TileIndex tile2 = child->node.tile;
				road_bits |= DiagDirToRoadBits(DiagdirBetweenTiles(tile, tile2));
			}
			if (path->parent != nullptr) {
				const TileIndex tile2 = path->parent->node.tile;
				road_bits |= DiagDirToRoadBits(DiagdirBetweenTiles(tile, tile2));
			}

			if (child != nullptr || path->parent != nullptr) {
				// Check if we need to build anything.
				bool need_to_build_road = true;

				if (IsTileType(tile, MP_ROAD)) {
					const RoadBits existing_bits = GetRoadBits(tile, RTT_ROAD);
					CLRBITS(road_bits, existing_bits);
					if (road_bits == ROAD_NONE) need_to_build_road = false;
				}

				// If it is already a road and has the right bits, we are good. Otherwise build the needed ones.
				if (need_to_build_road)
				{
					Backup cur_company(_current_company, OWNER_DEITY, FILE_LINE);
					CmdBuildRoad(tile, DC_EXEC, _public_road_type << 4 | road_bits, 0);
					cur_company.Restore();
				}
			}
		} else {
			// We only get here if we have a parent and we're not adjacent to it. River/Tunnel time!
			const DiagDirection road_direction = DiagdirBetweenTiles(tile, path->parent->node.tile);

			auto end_tile = INVALID_TILE;
			
			if (IsUpwardsSlope(tile, road_direction)) {
				end_tile = BuildTunnel(path, path->parent->node.tile, true);
				assert(IsValidTile(end_tile) && IsDownwardsSlope(end_tile, road_direction));
			} else if (IsDownwardsSlope(tile, road_direction)) {
				// Provide the function with the end tile, since we already know it, but still check the result.
				end_tile = BuildBridge(path, path->parent->node.tile, true);
				assert(IsValidTile(end_tile) && IsUpwardsSlope(end_tile, road_direction));
			} else {
				// River bridge is the last possibility.
				assert(GetTileSlope(tile) == SLOPE_FLAT);
				end_tile = BuildRiverBridge(path, road_direction, path->parent->node.tile, true);
				assert(IsValidTile(end_tile) && GetTileSlope(end_tile) == SLOPE_FLAT);
			}
		}

		child = path;
	}
}

bool FindPath(AyStar& finder, const TileIndex from, TileIndex to)
{
	finder.CalculateG = PublicRoad_CalculateG;
	finder.CalculateH = PublicRoad_CalculateH;
	finder.GetNeighbours = PublicRoad_GetNeighbours;
	finder.EndNodeCheck = PublicRoad_EndNodeCheck;
	finder.FoundEndNode = PublicRoad_FoundEndNode;
	finder.user_target = &(to);
	finder.max_search_nodes = 1 << 20; // 1,048,576

	finder.Init(1 << _public_road_hash_size);

	_has_tunnel_in_path = false;
	
	AyStarNode start {};
	start.tile = from;
	start.direction = INVALID_TRACKDIR;
	finder.AddStartNode(&start, 0);

	int result = AYSTAR_STILL_BUSY;
	
	while (result == AYSTAR_STILL_BUSY) {
		result = finder.Main();
	}

	const bool found_path = (result == AYSTAR_FOUND_END_NODE);

	return found_path;
}

/**
* Build the public road network connecting towns using AyStar.
*/
void GeneratePublicRoads()
{
	using namespace std;

	if (_settings_game.game_creation.build_public_roads == PRC_NONE) return;

	_town_centers.clear();
	_towns_visited_along_the_way.clear();

	vector<TileIndex> towns;
	towns.clear();
	{
		for (const Town *town : Town::Iterate()) {
			towns.push_back(town->xy);
			_town_centers.push_back(town->xy);
		}
	}

	if (towns.empty()) {
		return;
	}
	
	SetGeneratingWorldProgress(GWP_PUBLIC_ROADS, uint(towns.size()));

	// Create a list of networks which also contain a value indicating how many times we failed to connect to them.
	vector<pair<uint, shared_ptr<vector<TileIndex>>>> town_networks;
	unordered_map<TileIndex, shared_ptr<vector<TileIndex>>> towns_reachable_networks;

	sort(towns.begin(), towns.end(), [&](auto a, auto b) { return DistanceFromEdge(a) > DistanceFromEdge(b); });

	TileIndex main_town = *towns.begin();
	towns.erase(towns.begin());

	_public_road_type = GetTownRoadType(Town::GetByTile(main_town));

	auto main_network = make_shared<vector<TileIndex>>();
	main_network->push_back(main_town);

	town_networks.emplace_back(0, main_network);
	IncreaseGeneratingWorldProgress(GWP_PUBLIC_ROADS);

	sort(towns.begin(), towns.end(), [&](auto a, auto b) { return DistanceManhattan(main_town, a) < DistanceManhattan(main_town, b); });

	for (auto begin_town : towns) {
		// Check if we can connect to any of the networks.
		_towns_visited_along_the_way.clear();

		auto reachable_network_iter = towns_reachable_networks.find(begin_town);
		bool found_easy_path = false;

		if (reachable_network_iter != towns_reachable_networks.end()) {
			auto reachable_network = reachable_network_iter->second;

			sort(reachable_network->begin(), reachable_network->end(), [&](auto a, auto b) { return DistanceManhattan(begin_town, a) < DistanceManhattan(begin_town, b); });

			const TileIndex end_town = *reachable_network->begin();

			AyStar finder {};

			found_easy_path = FindPath(finder, begin_town, end_town);

			finder.Free();
		}

		if (found_easy_path) {
			reachable_network_iter->second->push_back(begin_town);

			for (const TileIndex visited_town : _towns_visited_along_the_way) {
				if (visited_town != begin_town) towns_reachable_networks[visited_town] = reachable_network_iter->second;
			}
		} else {
			// Sort networks by failed connection attempts, so we try the most likely one first.
			sort(town_networks.begin(), town_networks.end(), [&](auto a, auto b) { return a.first < b.first; });

			std::function<bool(pair<uint, shared_ptr<vector<TileIndex>>>)> can_reach_network = [&](auto network_pair) {
				AyStar finder {};

				auto network = network_pair.second;

				// Try to connect to the town in the network that is closest to us.
				// If we can't connect to that one, we can't connect to any of them since they are all interconnected.
				sort(network->begin(), network->end(), [&](auto a, auto b) { return DistanceManhattan(begin_town, a) < DistanceManhattan(begin_town, b); });
				const TileIndex end_town = *network->begin();

				const bool found_path = FindPath(finder, begin_town, end_town);

				if (found_path) {
					network->push_back(begin_town);

					for (auto visited_town : _towns_visited_along_the_way) {
						if (visited_town != begin_town) towns_reachable_networks[visited_town] = network;
					}
				}

				// Increase number of failed attempts if necessary.
				network_pair.first += (found_path ? (network_pair.first > 0 ? -1 : 0) : 1);

				finder.Free();

				return found_path;

			};

			if (!any_of(town_networks.begin(), town_networks.end(), can_reach_network)) {
				// We failed to connect to any network, so we are a separate network. Let future towns try to connect to us.
				auto new_network = make_shared<vector<TileIndex>>();
				new_network->push_back(begin_town);

				// We basically failed to connect to this many towns.
				int towns_already_in_networks = std::accumulate(town_networks.begin(), town_networks.end(), 0, [&](int accumulator, auto network_pair) {
					return accumulator + static_cast<int>(network_pair.second->size());
				});

				town_networks.emplace_back(towns_already_in_networks, new_network);

				for (const TileIndex visited_town : _towns_visited_along_the_way) {
					if (visited_town != begin_town) towns_reachable_networks.insert(make_pair(visited_town, new_network));
				}
			}
		}

		IncreaseGeneratingWorldProgress(GWP_PUBLIC_ROADS);
	}
}

/* ========================================================================= */
/*                              END PUBLIC ROADS                             */
/* ========================================================================= */

/**
 * Get list of road types, regardless of company availability.
 * @param introduces If true, include road types introduced by other road types
 * @return the road types.
 */
RoadTypes GetRoadTypes(bool introduces)
{
	RoadTypes rts = ROADTYPES_NONE;

	for (const Engine *e : Engine::IterateType(VEH_ROAD)) {
		const EngineInfo *ei = &e->info;
		if (!HasBit(ei->climates, _settings_game.game_creation.landscape)) continue;

		const RoadVehicleInfo *rvi = &e->u.road;
		assert(rvi->roadtype < ROADTYPE_END);
		if (introduces) {
			rts |= GetRoadTypeInfo(rvi->roadtype)->introduces_roadtypes;
		} else {
			SetBit(rts, rvi->roadtype);
		}
	}

	if (introduces) return AddDateIntroducedRoadTypes(rts, MAX_DAY);
	return rts;
}

/**
 * Get the road type for a given label.
 * @param label the roadtype label.
 * @param allow_alternate_labels Search in the alternate label lists as well.
 * @return the roadtype.
 */
RoadType GetRoadTypeByLabel(RoadTypeLabel label, bool allow_alternate_labels)
{
	/* Loop through each road type until the label is found */
	for (RoadType r = ROADTYPE_BEGIN; r != ROADTYPE_END; r++) {
		const RoadTypeInfo *rti = GetRoadTypeInfo(r);
		if (rti->label == label) return r;
	}

	if (allow_alternate_labels) {
		/* Test if any road type defines the label as an alternate. */
		for (RoadType r = ROADTYPE_BEGIN; r != ROADTYPE_END; r++) {
			const RoadTypeInfo *rti = GetRoadTypeInfo(r);
			if (std::find(rti->alternate_labels.begin(), rti->alternate_labels.end(), label) != rti->alternate_labels.end()) return r;
		}
	}

	/* No matching label was found, so it is invalid */
	return INVALID_ROADTYPE;
}

/**
 * Returns the available RoadSubTypes for the provided RoadType
 * If the given company is valid then will be returned a list of the available sub types at the current date, while passing
 * a deity company will make all the sub types available
 * @param rt the RoadType to filter
 * @param c the company ID to check the roadtype against
 * @param any_date whether to return only currently introduced roadtypes or also future ones
 * @returns the existing RoadSubTypes
 */
RoadTypes ExistingRoadTypes(CompanyID c)
{
	/* Check only players which can actually own vehicles, editor and gamescripts are considered deities */
	if (c < OWNER_END) {
		const Company *company = Company::GetIfValid(c);
		if (company != nullptr) return company->avail_roadtypes;
	}

	RoadTypes known_roadtypes = ROADTYPES_NONE;

	/* Find used roadtypes */
	for (Engine *e : Engine::IterateType(VEH_ROAD)) {
		/* Check if the roadtype can be used in the current climate */
		if (!HasBit(e->info.climates, _settings_game.game_creation.landscape)) continue;

		/* Check whether available for all potential companies */
		if (e->company_avail != (CompanyMask)-1) continue;

		known_roadtypes |= GetRoadTypeInfo(e->u.road.roadtype)->introduces_roadtypes;
	}

	/* Get the date introduced roadtypes as well. */
	known_roadtypes = AddDateIntroducedRoadTypes(known_roadtypes, MAX_DAY);

	return known_roadtypes;
}
