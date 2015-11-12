/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file newgrf_house.cpp Implementation of NewGRF houses. */

#include "stdafx.h"
#include "debug.h"
#include "landscape.h"
#include "newgrf_house.h"
#include "newgrf_spritegroup.h"
#include "newgrf_town.h"
#include "newgrf_sound.h"
#include "company_func.h"
#include "company_base.h"
#include "town.h"
#include "genworld.h"
#include "newgrf_animation_base.h"
#include "newgrf_cargo.h"
#include "station_base.h"

static BuildingCounts<uint32> _building_counts;
static HouseClassMapping _class_mapping[HOUSE_CLASS_MAX];

HouseOverrideManager _house_mngr(NEW_HOUSE_OFFSET, NUM_HOUSES, INVALID_HOUSE_ID);

/**
 * Constructor of a house scope resolver.
 * @param grffile GRFFile the resolved SpriteGroup belongs to.
 * @param house_id House type being queried.
 * @param tile %Tile containing the house.
 * @param town %Town containing the house.
 * @param not_yet_constructed House is still under construction.
 * @param initial_random_bits Random bits during construction checks.
 * @param watched_cargo_triggers Cargo types that triggered the watched cargo callback.
 */
HouseScopeResolver::HouseScopeResolver (const GRFFile *grffile, HouseID house_id, TileIndex tile, Town *town,
			bool not_yet_constructed, uint8 initial_random_bits, uint32 watched_cargo_triggers)
	: ScopeResolver(), grffile(grffile)
{
	this->house_id = house_id;
	this->tile = tile;
	this->town = town;
	this->not_yet_constructed = not_yet_constructed;
	this->initial_random_bits = initial_random_bits;
	this->watched_cargo_triggers = watched_cargo_triggers;
}

/**
 * Retrieve the grf file associated with a house.
 * @param house_id House to query.
 * @return The associated GRF file (may be \c NULL).
 */
static const GRFFile *GetHouseSpecGrf(HouseID house_id)
{
	const HouseSpec *hs  = HouseSpec::Get(house_id);
	return (hs != NULL) ? hs->grf_prop.grffile : NULL;
}

/**
 * Construct a resolver for a house.
 * @param house_id House to query.
 * @param tile %Tile containing the house.
 * @param town %Town containing the house.
 * @param callback Callback ID.
 * @param param1 First parameter (var 10) of the callback.
 * @param param2 Second parameter (var 18) of the callback.
 * @param not_yet_constructed House is still under construction.
 * @param initial_random_bits Random bits during construction checks.
 * @param watched_cargo_triggers Cargo types that triggered the watched cargo callback.
 */
HouseResolverObject::HouseResolverObject(HouseID house_id, TileIndex tile, Town *town,
		CallbackID callback, uint32 param1, uint32 param2,
		bool not_yet_constructed, uint8 initial_random_bits, uint32 watched_cargo_triggers)
	: ResolverObject(GetHouseSpecGrf(house_id), callback, param1, param2),
	  house_scope (this->grffile, house_id, tile, town, not_yet_constructed, initial_random_bits, watched_cargo_triggers),
	  town_scope (this->grffile, town, not_yet_constructed) // Don't access StorePSA if house is not yet constructed.
{
	this->root_spritegroup = HouseSpec::Get(house_id)->grf_prop.spritegroup[0];
}

/**
 * Construct a fake resolver for a house.
 * @param house_id House to query.
 * @param tile %Tile containing the house.
 * @param town %Town containing the house.
 * @param callback Callback ID.
 * @param param1 First parameter (var 10) of the callback.
 * @param param2 Second parameter (var 18) of the callback.
 * @param not_yet_constructed House is still under construction.
 * @param initial_random_bits Random bits during construction checks.
 * @param watched_cargo_triggers Cargo types that triggered the watched cargo callback.
 */
FakeHouseResolverObject::FakeHouseResolverObject (HouseID house_id,
		CallbackID callback, uint32 param1, uint32 param2)
	: ResolverObject (GetHouseSpecGrf(house_id), callback, param1, param2),
	  house_scope (house_id), town_scope()
{
	this->root_spritegroup = HouseSpec::Get(house_id)->grf_prop.spritegroup[0];
}

HouseClassID AllocateHouseClassID(byte grf_class_id, uint32 grfid)
{
	/* Start from 1 because 0 means that no class has been assigned. */
	for (int i = 1; i != lengthof(_class_mapping); i++) {
		HouseClassMapping *map = &_class_mapping[i];

		if (map->class_id == grf_class_id && map->grfid == grfid) return (HouseClassID)i;

		if (map->class_id == 0 && map->grfid == 0) {
			map->class_id = grf_class_id;
			map->grfid    = grfid;
			return (HouseClassID)i;
		}
	}
	return HOUSE_NO_CLASS;
}

void InitializeBuildingCounts()
{
	memset(&_building_counts, 0, sizeof(_building_counts));

	Town *t;
	FOR_ALL_TOWNS(t) {
		memset(&t->cache.building_counts, 0, sizeof(t->cache.building_counts));
	}
}

/**
 * IncreaseBuildingCount()
 * Increase the count of a building when it has been added by a town.
 * @param t The town that the building is being built in
 * @param house_id The id of the house being added
 */
void IncreaseBuildingCount(Town *t, HouseID house_id)
{
	HouseClassID class_id = HouseSpec::Get(house_id)->class_id;

	if (!_loaded_newgrf_features.has_newhouses) return;

	t->cache.building_counts.id_count[house_id]++;
	_building_counts.id_count[house_id]++;

	if (class_id == HOUSE_NO_CLASS) return;

	t->cache.building_counts.class_count[class_id]++;
	_building_counts.class_count[class_id]++;
}

/**
 * DecreaseBuildingCount()
 * Decrease the number of a building when it is deleted.
 * @param t The town that the building was built in
 * @param house_id The id of the house being removed
 */
void DecreaseBuildingCount(Town *t, HouseID house_id)
{
	HouseClassID class_id = HouseSpec::Get(house_id)->class_id;

	if (!_loaded_newgrf_features.has_newhouses) return;

	if (t->cache.building_counts.id_count[house_id] > 0) t->cache.building_counts.id_count[house_id]--;
	if (_building_counts.id_count[house_id] > 0) _building_counts.id_count[house_id]--;

	if (class_id == HOUSE_NO_CLASS) return;

	if (t->cache.building_counts.class_count[class_id] > 0) t->cache.building_counts.class_count[class_id]--;
	if (_building_counts.class_count[class_id] > 0) _building_counts.class_count[class_id]--;
}

/* virtual */ uint32 HouseScopeResolver::GetRandomBits() const
{
	/* Note: Towns build houses over houses. So during construction checks 'tile' may be a valid but unrelated house. */
	assert(IsValidTile(this->tile) && (this->not_yet_constructed || IsHouseTile(this->tile)));
	return this->not_yet_constructed ? this->initial_random_bits : GetHouseRandomBits(this->tile);
}

/* virtual */ uint32 HouseScopeResolver::GetTriggers() const
{
	/* Note: Towns build houses over houses. So during construction checks 'tile' may be a valid but unrelated house. */
	assert(IsValidTile(this->tile) && (this->not_yet_constructed || IsHouseTile(this->tile)));
	return this->not_yet_constructed ? 0 : GetHouseTriggers(this->tile);
}

/* virtual */ void HouseScopeResolver::SetTriggers(int triggers) const
{
	assert(!this->not_yet_constructed && IsValidTile(this->tile) && IsHouseTile(this->tile));
	SetHouseTriggers(this->tile, triggers);
}

static uint32 GetNumHouses(HouseID house_id, const Town *town)
{
	uint8 map_id_count, town_id_count, map_class_count, town_class_count;
	HouseClassID class_id = HouseSpec::Get(house_id)->class_id;

	map_id_count     = ClampU(_building_counts.id_count[house_id], 0, 255);
	map_class_count  = ClampU(_building_counts.class_count[class_id], 0, 255);
	town_id_count    = ClampU(town->cache.building_counts.id_count[house_id], 0, 255);
	town_class_count = ClampU(town->cache.building_counts.class_count[class_id], 0, 255);

	return map_class_count << 24 | town_class_count << 16 | map_id_count << 8 | town_id_count;
}

/**
 * Get information about a nearby tile.
 * @param parameter from callback. It's in fact a pair of coordinates
 * @param tile TileIndex from which the callback was initiated
 * @param grf_version8 True, if we are dealing with a new NewGRF which uses GRF version >= 8.
 * @return a construction of bits obeying the newgrf format
 */
static uint32 GetNearbyTileInformation(byte parameter, TileIndex tile, bool grf_version8)
{
	tile = GetNearbyTile(parameter, tile);
	return GetNearbyTileInformation(tile, grf_version8);
}

/**
 * This function will activate a search around a central tile, looking for some houses
 * that fit the requested characteristics
 * @param parameter that is given by the callback.
 *                  bits 0..6 radius of the search
 *                  bits 7..8 search type i.e.: 0 = houseID/ 1 = classID/ 2 = grfID
 * @param tile TileIndex from which to start the search
 * @param house the HouseID that is associated to the house, the callback is called for
 * @return the Manhattan distance from the center tile, if any, and 0 if failure
 */
static uint32 GetDistanceFromNearbyHouse(uint8 parameter, TileIndex tile, HouseID house)
{
	enum SearchType {
		SEARCH_BY_ID,
		SEARCH_BY_CLASS,
		SEARCH_BY_GRFID,
		SEARCH_BY_END,
	};

	uint8 searchtype = GB(parameter, 6, 2);
	uint8 searchradius = GB(parameter, 0, 6);

	if (searchtype >= SEARCH_BY_END) return 0;  // do not run on ill-defined code
	if (searchradius < 1) return 0; // do not use a too low radius

	const HouseSpec *orig_hs = HouseSpec::Get(house);
	TileIndex orig_north_tile = tile + GetHouseNorthPart(house); // modifies 'house'!

	CircularTileIterator iter (tile, 2 * searchradius + 1);
	for (TileIndex t = iter; t != INVALID_TILE; t = ++iter) {
		if (!IsHouseTile(t)) continue;

		HouseID house = GetHouseType(t);
		const HouseSpec *hs = HouseSpec::Get(house);

		/* house must be from the same grf file */
		if (hs->grf_prop.grffile == NULL) continue;
		if (hs->grf_prop.grffile->grfid != orig_hs->grf_prop.grffile->grfid) continue;

		TileIndex north = t + GetHouseNorthPart(house); // modifies 'house'!
		/* always ignore origin house */
		if (north == orig_north_tile) continue;

		bool match;
		switch (searchtype) {
			case SEARCH_BY_ID:
				match = (hs->grf_prop.local_id == orig_hs->grf_prop.local_id); // same local id as the one requested
				break;

			case SEARCH_BY_CLASS:
				match = (hs->class_id == orig_hs->class_id); // same classid as the one requested
				break;

			case SEARCH_BY_GRFID:
				match = true; // from the same grf
				break;

			default: NOT_REACHED();
		}

		if (match) return DistanceManhattan (t, tile);
	}
	return 0;
}

/**
 * @note Used by the resolver to get values for feature 07 deterministic spritegroups.
 */
/* virtual */ uint32 HouseScopeResolver::GetVariable(byte variable, uint32 parameter, bool *available) const
{
	switch (variable) {
		/* Construction stage. */
		case 0x40: return (IsHouseTile(this->tile) ? GetHouseBuildingStage(this->tile) : 0) | TileHash2Bit(TileX(this->tile), TileY(this->tile)) << 2;

		/* Building age. */
		case 0x41: return IsHouseTile(this->tile) ? GetHouseAge(this->tile) : 0;

		/* Town zone */
		case 0x42: return GetTownRadiusGroup(this->town, this->tile);

		/* Terrain type */
		case 0x43: return GetTerrainType(this->tile);

		/* Number of this type of building on the map. */
		case 0x44: return GetNumHouses(this->house_id, this->town);

		/* Whether the town is being created or just expanded. */
		case 0x45: return _generating_world ? 1 : 0;

		/* Current animation frame. */
		case 0x46: return IsHouseTile(this->tile) ? GetAnimationFrame(this->tile) : 0;

		/* Position of the house */
		case 0x47: return TileY(this->tile) << 16 | TileX(this->tile);

		/* Building counts for old houses with id = parameter. */
		case 0x60: return parameter < NEW_HOUSE_OFFSET ? GetNumHouses(parameter, this->town) : 0;

		/* Building counts for new houses with id = parameter. */
		case 0x61: {
			const HouseSpec *hs = HouseSpec::Get(this->house_id);
			if (hs->grf_prop.grffile == NULL) return 0;

			HouseID new_house = _house_mngr.GetID(parameter, hs->grf_prop.grffile->grfid);
			return new_house == INVALID_HOUSE_ID ? 0 : GetNumHouses(new_house, this->town);
		}

		/* Land info for nearby tiles. */
		case 0x62: return GetNearbyTileInformation (parameter, this->tile, this->grffile->grf_version >= 8);

		/* Current animation frame of nearby house tiles */
		case 0x63: {
			TileIndex testtile = GetNearbyTile(parameter, this->tile);
			return IsHouseTile(testtile) ? GetAnimationFrame(testtile) : 0;
		}

		/* Cargo acceptance history of nearby stations */
		case 0x64: {
			CargoID cid = GetCargoTranslation (parameter, this->grffile);
			if (cid == CT_INVALID) return 0;

			/* Extract tile offset. */
			int8 x_offs = GB(GetRegister(0x100), 0, 8);
			int8 y_offs = GB(GetRegister(0x100), 8, 8);
			TileIndex testtile = TILE_MASK(this->tile + TileDiffXY(x_offs, y_offs));

			StationFinder stations(TileArea(testtile, 1, 1));
			const StationList *sl = stations.GetStations();

			/* Collect acceptance stats. */
			uint32 res = 0;
			for (Station * const * st_iter = sl->Begin(); st_iter != sl->End(); st_iter++) {
				const Station *st = *st_iter;
				if (HasBit(st->goods[cid].status, GoodsEntry::GES_EVER_ACCEPTED))    SetBit(res, 0);
				if (HasBit(st->goods[cid].status, GoodsEntry::GES_LAST_MONTH))       SetBit(res, 1);
				if (HasBit(st->goods[cid].status, GoodsEntry::GES_CURRENT_MONTH))    SetBit(res, 2);
				if (HasBit(st->goods[cid].status, GoodsEntry::GES_ACCEPTED_BIGTICK)) SetBit(res, 3);
			}

			/* Cargo triggered CB 148? */
			if (HasBit(this->watched_cargo_triggers, cid)) SetBit(res, 4);

			return res;
		}

		/* Distance test for some house types */
		case 0x65: return GetDistanceFromNearbyHouse(parameter, this->tile, this->house_id);

		/* Class and ID of nearby house tile */
		case 0x66: {
			TileIndex testtile = GetNearbyTile(parameter, this->tile);
			if (!IsHouseTile(testtile)) return 0xFFFFFFFF;
			HouseSpec *hs = HouseSpec::Get(GetHouseType(testtile));
			/* Information about the grf local classid if the house has a class */
			uint houseclass = 0;
			if (hs->class_id != HOUSE_NO_CLASS) {
				houseclass = (hs->grf_prop.grffile == this->grffile ? 1 : 2) << 8;
				houseclass |= _class_mapping[hs->class_id].class_id;
			}
			/* old house type or grf-local houseid */
			uint local_houseid = 0;
			if (this->house_id < NEW_HOUSE_OFFSET) {
				local_houseid = this->house_id;
			} else {
				local_houseid = (hs->grf_prop.grffile == this->grffile ? 1 : 2) << 8;
				local_houseid |= hs->grf_prop.local_id;
			}
			return houseclass << 16 | local_houseid;
		}

		/* GRFID of nearby house tile */
		case 0x67: {
			TileIndex testtile = GetNearbyTile(parameter, this->tile);
			if (!IsHouseTile(testtile)) return 0xFFFFFFFF;
			HouseID house_id = GetHouseType(testtile);
			if (house_id < NEW_HOUSE_OFFSET) return 0;
			/* Checking the grffile information via HouseSpec doesn't work
			 * in case the newgrf was removed. */
			return _house_mngr.GetGRFID(house_id);
		}
	}

	DEBUG(grf, 1, "Unhandled house variable 0x%X", variable);

	*available = false;
	return UINT_MAX;
}


/**
 * @note Used by the resolver to get values for feature 07 deterministic spritegroups.
 */
/* virtual */ uint32 FakeHouseScopeResolver::GetVariable(byte variable, uint32 parameter, bool *available) const
{
	switch (variable) {
		/* Construction stage. */
		case 0x40: return TOWN_HOUSE_COMPLETED;

		/* Building age. */
		case 0x41: return 0;

		/* Town zone */
		case 0x42: return FIND_FIRST_BIT(HouseSpec::Get(this->house_id)->building_availability & HZ_ZONALL); // first available

		/* Terrain type */
		case 0x43: return _settings_game.game_creation.landscape == LT_ARCTIC && (HouseSpec::Get(house_id)->building_availability & (HZ_SUBARTC_ABOVE | HZ_SUBARTC_BELOW)) == HZ_SUBARTC_ABOVE ? 4 : 0;

		/* Number of this type of building on the map. */
		case 0x44: return 0;

		/* Whether the town is being created or just expanded. */
		case 0x45: return 0;

		/* Current animation frame. */
		case 0x46: return 0;

		/* Position of the house */
		case 0x47: return 0xFFFFFFFF;

		/* Building counts for old houses with id = parameter. */
		case 0x60: return 0;

		/* Building counts for new houses with id = parameter. */
		case 0x61: return 0;

		/* Land info for nearby tiles. */
		case 0x62: return 0;

		/* Current animation frame of nearby house tiles */
		case 0x63: return 0;

		/* Cargo acceptance history of nearby stations */
		case 0x64: return 0;

		/* Distance test for some house types */
		case 0x65: return 0;

		/* Class and ID of nearby house tile */
		case 0x66: return 0xFFFFFFFF;

		/* GRFID of nearby house tile */
		case 0x67: return 0xFFFFFFFF;
	}

	DEBUG(grf, 1, "Unhandled house variable 0x%X", variable);

	*available = false;
	return UINT_MAX;
}

uint16 GetHouseCallback(CallbackID callback, uint32 param1, uint32 param2, HouseID house_id, Town *town, TileIndex tile,
		bool not_yet_constructed, uint8 initial_random_bits, uint32 watched_cargo_triggers)
{
	assert(IsValidTile(tile) && (not_yet_constructed || IsHouseTile(tile)));

	HouseResolverObject object(house_id, tile, town, callback, param1, param2,
			not_yet_constructed, initial_random_bits, watched_cargo_triggers);
	return SpriteGroup::CallbackResult (object.Resolve());
}

uint16 GetHouseCallback (CallbackID callback, uint32 param1, uint32 param2, HouseID house_id)
{
	FakeHouseResolverObject object (house_id, callback, param1, param2);
	return SpriteGroup::CallbackResult (object.Resolve());
}

static void DrawTileLayout(const TileInfo *ti, const TileLayoutSpriteGroup *group, byte stage, HouseID house_id)
{
	const DrawTileSprites *dts = group->ProcessRegisters(&stage);

	const HouseSpec *hs = HouseSpec::Get(house_id);
	PaletteID palette = hs->random_colour[TileHash2Bit(ti->x, ti->y)] + PALETTE_RECOLOUR_START;
	if (HasBit(hs->callback_mask, CBM_HOUSE_COLOUR)) {
		uint16 callback = GetHouseCallback(CBID_HOUSE_COLOUR, 0, 0, house_id, Town::GetByTile(ti->tile), ti->tile);
		if (callback != CALLBACK_FAILED) {
			/* If bit 14 is set, we should use a 2cc colour map, else use the callback value. */
			palette = HasBit(callback, 14) ? GB(callback, 0, 8) + SPR_2CCMAP_BASE : callback;
		}
	}

	SpriteID image = dts->ground.sprite;
	PaletteID pal  = dts->ground.pal;

	if (HasBit(image, SPRITE_MODIFIER_CUSTOM_SPRITE)) image += stage;
	if (HasBit(pal, SPRITE_MODIFIER_CUSTOM_SPRITE)) pal += stage;

	if (GB(image, 0, SPRITE_WIDTH) != 0) {
		DrawGroundSprite(image, GroundSpritePaletteTransform(image, pal, palette));
	}

	DrawNewGRFTileSeq(ti, dts, TO_HOUSES, stage, palette);
}

static void DrawTileLayoutInGUI(int x, int y, const TileLayoutSpriteGroup *group, HouseID house_id, bool ground)
{
	byte stage = TOWN_HOUSE_COMPLETED;
	const DrawTileSprites *dts = group->ProcessRegisters(&stage);

	const HouseSpec *hs = HouseSpec::Get(house_id);
	PaletteID palette = hs->random_colour[0] + PALETTE_RECOLOUR_START;
	if (HasBit(hs->callback_mask, CBM_HOUSE_COLOUR)) {
		uint16 callback = GetHouseCallback(CBID_HOUSE_COLOUR, 0, 0, house_id);
		if (callback != CALLBACK_FAILED) {
			/* If bit 14 is set, we should use a 2cc colour map, else use the callback value. */
			palette = HasBit(callback, 14) ? GB(callback, 0, 8) + SPR_2CCMAP_BASE : callback;
		}
	}

	if (ground) {
		PalSpriteID image = dts->ground;
		if (HasBit(image.sprite, SPRITE_MODIFIER_CUSTOM_SPRITE)) image.sprite += stage;
		if (HasBit(image.pal, SPRITE_MODIFIER_CUSTOM_SPRITE)) image.pal += stage;

		if (GB(image.sprite, 0, SPRITE_WIDTH) != 0) {
			DrawSprite(image.sprite, GroundSpritePaletteTransform(image.sprite, image.pal, palette), x, y);
		}
	} else {
		DrawNewGRFTileSeqInGUI(x, y, dts, stage, palette);
	}
}

void DrawNewHouseTile(TileInfo *ti, HouseID house_id)
{
	const HouseSpec *hs = HouseSpec::Get(house_id);

	if (ti->tileh != SLOPE_FLAT) {
		bool draw_old_one = true;
		if (HasBit(hs->callback_mask, CBM_HOUSE_DRAW_FOUNDATIONS)) {
			/* Called to determine the type (if any) of foundation to draw for the house tile */
			uint32 callback_res = GetHouseCallback(CBID_HOUSE_DRAW_FOUNDATIONS, 0, 0, house_id, Town::GetByTile(ti->tile), ti->tile);
			if (callback_res != CALLBACK_FAILED) draw_old_one = ConvertBooleanCallback(hs->grf_prop.grffile, CBID_HOUSE_DRAW_FOUNDATIONS, callback_res);
		}

		if (draw_old_one) DrawFoundation(ti, FOUNDATION_LEVELED);
	}

	HouseResolverObject object(house_id, ti->tile, Town::GetByTile(ti->tile));

	const SpriteGroup *group = object.Resolve();
	if (group != NULL && group->type == SGT_TILELAYOUT) {
		/* Limit the building stage to the number of stages supplied. */
		const TileLayoutSpriteGroup *tlgroup = (const TileLayoutSpriteGroup *)group;
		byte stage = GetHouseBuildingStage(ti->tile);
		DrawTileLayout(ti, tlgroup, stage, house_id);
	}
}

void DrawNewHouseTileInGUI(int x, int y, HouseID house_id, bool ground)
{
	FakeHouseResolverObject object(house_id);
	const SpriteGroup *group = object.Resolve();
	if (group != NULL && group->type == SGT_TILELAYOUT) {
		DrawTileLayoutInGUI(x, y, (const TileLayoutSpriteGroup*)group, house_id, ground);
	}
}

/* Simple wrapper for GetHouseCallback to keep the animation unified. */
uint16 GetSimpleHouseCallback(CallbackID callback, uint32 param1, uint32 param2, const HouseSpec *spec, Town *town, TileIndex tile, uint32 extra_data)
{
	return GetHouseCallback(callback, param1, param2, spec - HouseSpec::Get(0), town, tile, false, 0, extra_data);
}

/** Helper class for animation control. */
struct HouseAnimationBase : public AnimationBase<HouseAnimationBase, HouseSpec, Town, uint32, GetSimpleHouseCallback> {
	static const CallbackID cb_animation_speed      = CBID_HOUSE_ANIMATION_SPEED;
	static const CallbackID cb_animation_next_frame = CBID_HOUSE_ANIMATION_NEXT_FRAME;

	static const HouseCallbackMask cbm_animation_speed      = CBM_HOUSE_ANIMATION_SPEED;
	static const HouseCallbackMask cbm_animation_next_frame = CBM_HOUSE_ANIMATION_NEXT_FRAME;
};

void AnimateNewHouseTile(TileIndex tile)
{
	const HouseSpec *hs = HouseSpec::Get(GetHouseType(tile));
	if (hs == NULL) return;

	HouseAnimationBase::AnimateTile(hs, Town::GetByTile(tile), tile, HasBit(hs->extra_flags, CALLBACK_1A_RANDOM_BITS));
}

void AnimateNewHouseConstruction(TileIndex tile)
{
	const HouseSpec *hs = HouseSpec::Get(GetHouseType(tile));

	if (HasBit(hs->callback_mask, CBM_HOUSE_CONSTRUCTION_STATE_CHANGE)) {
		HouseAnimationBase::ChangeAnimationFrame(CBID_HOUSE_CONSTRUCTION_STATE_CHANGE, hs, Town::GetByTile(tile), tile, 0, 0);
	}
}

bool CanDeleteHouse(TileIndex tile)
{
	const HouseSpec *hs = HouseSpec::Get(GetHouseType(tile));

	/* Humans are always allowed to remove buildings, as is water and disasters and
	 * anyone using the scenario editor. */
	if (Company::IsValidHumanID(_current_company) || _current_company == OWNER_WATER || _current_company == OWNER_NONE || _game_mode == GM_EDITOR || _generating_world) {
		return true;
	}

	if (HasBit(hs->callback_mask, CBM_HOUSE_DENY_DESTRUCTION)) {
		uint16 callback_res = GetHouseCallback(CBID_HOUSE_DENY_DESTRUCTION, 0, 0, GetHouseType(tile), Town::GetByTile(tile), tile);
		return (callback_res == CALLBACK_FAILED || !ConvertBooleanCallback(hs->grf_prop.grffile, CBID_HOUSE_DENY_DESTRUCTION, callback_res));
	} else {
		return !(hs->extra_flags & BUILDING_IS_PROTECTED);
	}
}

static void AnimationControl(TileIndex tile, uint16 random_bits)
{
	const HouseSpec *hs = HouseSpec::Get(GetHouseType(tile));

	if (HasBit(hs->callback_mask, CBM_HOUSE_ANIMATION_START_STOP)) {
		uint32 param = (hs->extra_flags & SYNCHRONISED_CALLBACK_1B) ? (GB(Random(), 0, 16) | random_bits << 16) : Random();
		HouseAnimationBase::ChangeAnimationFrame(CBID_HOUSE_ANIMATION_START_STOP, hs, Town::GetByTile(tile), tile, param, 0);
	}
}

bool NewHouseTileLoop(TileIndex tile)
{
	const HouseSpec *hs = HouseSpec::Get(GetHouseType(tile));

	if (GetHouseProcessingTime(tile) > 0) {
		DecHouseProcessingTime(tile);
		return true;
	}

	TriggerHouse(tile, HOUSE_TRIGGER_TILE_LOOP);
	if (hs->building_flags & BUILDING_HAS_1_TILE) TriggerHouse(tile, HOUSE_TRIGGER_TILE_LOOP_TOP);

	if (HasBit(hs->callback_mask, CBM_HOUSE_ANIMATION_START_STOP)) {
		/* If this house is marked as having a synchronised callback, all the
		 * tiles will have the callback called at once, rather than when the
		 * tile loop reaches them. This should only be enabled for the northern
		 * tile, or strange things will happen (here, and in TTDPatch). */
		if (hs->extra_flags & SYNCHRONISED_CALLBACK_1B) {
			uint16 random = GB(Random(), 0, 16);

			if (hs->building_flags & BUILDING_HAS_1_TILE)  AnimationControl(tile, random);
			if (hs->building_flags & BUILDING_2_TILES_Y)   AnimationControl(TILE_ADDXY(tile, 0, 1), random);
			if (hs->building_flags & BUILDING_2_TILES_X)   AnimationControl(TILE_ADDXY(tile, 1, 0), random);
			if (hs->building_flags & BUILDING_HAS_4_TILES) AnimationControl(TILE_ADDXY(tile, 1, 1), random);
		} else {
			AnimationControl(tile, 0);
		}
	}

	/* Check callback 21, which determines if a house should be destroyed. */
	if (HasBit(hs->callback_mask, CBM_HOUSE_DESTRUCTION)) {
		uint16 callback_res = GetHouseCallback(CBID_HOUSE_DESTRUCTION, 0, 0, GetHouseType(tile), Town::GetByTile(tile), tile);
		if (callback_res != CALLBACK_FAILED && Convert8bitBooleanCallback(hs->grf_prop.grffile, CBID_HOUSE_DESTRUCTION, callback_res)) {
			ClearTownHouse(Town::GetByTile(tile), tile);
			return false;
		}
	}

	SetHouseProcessingTime(tile, hs->processing_time);
	MarkTileDirtyByTile(tile);
	return true;
}

static void DoTriggerHouse(TileIndex tile, HouseTrigger trigger, byte base_random, bool first)
{
	/* We can't trigger a non-existent building... */
	assert(IsHouseTile(tile));

	HouseID hid = GetHouseType(tile);
	HouseSpec *hs = HouseSpec::Get(hid);

	if (hs->grf_prop.spritegroup[0] == NULL) return;

	HouseResolverObject object(hid, tile, Town::GetByTile(tile), CBID_RANDOM_TRIGGER);
	object.trigger = trigger;

	const SpriteGroup *group = object.Resolve();
	if (group == NULL) return;

	byte new_random_bits = Random();
	byte random_bits = GetHouseRandomBits(tile);
	uint32 reseed = object.GetReseedSum(); // The scope only affects triggers, not the reseeding
	random_bits &= ~reseed;
	random_bits |= (first ? new_random_bits : base_random) & reseed;
	SetHouseRandomBits(tile, random_bits);

	switch (trigger) {
		case HOUSE_TRIGGER_TILE_LOOP:
			/* Random value already set. */
			break;

		case HOUSE_TRIGGER_TILE_LOOP_TOP:
			if (!first) {
				/* The top tile is marked dirty by the usual TileLoop */
				MarkTileDirtyByTile(tile);
				break;
			}
			/* Random value of first tile already set. */
			if (hs->building_flags & BUILDING_2_TILES_Y)   DoTriggerHouse(TILE_ADDXY(tile, 0, 1), trigger, random_bits, false);
			if (hs->building_flags & BUILDING_2_TILES_X)   DoTriggerHouse(TILE_ADDXY(tile, 1, 0), trigger, random_bits, false);
			if (hs->building_flags & BUILDING_HAS_4_TILES) DoTriggerHouse(TILE_ADDXY(tile, 1, 1), trigger, random_bits, false);
			break;
	}
}

void TriggerHouse(TileIndex t, HouseTrigger trigger)
{
	DoTriggerHouse(t, trigger, 0, true);
}

/**
 * Run the watched cargo accepted callback for a single house tile.
 * @param tile The house tile.
 * @param origin The triggering tile.
 * @param trigger_cargoes Cargo types that triggered the callback.
 * @param random Random bits.
 */
void DoWatchedCargoCallback(TileIndex tile, TileIndex origin, uint32 trigger_cargoes, uint16 random)
{
	CoordDiff diff = TileCoordDiff(origin, tile);
	uint32 cb_info = random << 16 | (uint8)diff.y << 8 | (uint8)diff.x;
	HouseAnimationBase::ChangeAnimationFrame(CBID_HOUSE_WATCHED_CARGO_ACCEPTED, HouseSpec::Get(GetHouseType(tile)), Town::GetByTile(tile), tile, 0, cb_info, trigger_cargoes);
}

/**
 * Run watched cargo accepted callback for a house.
 * @param tile House tile.
 * @param trigger_cargoes Triggering cargo types.
 * @pre IsHouseTile(t)
 */
void WatchedCargoCallback(TileIndex tile, uint32 trigger_cargoes)
{
	assert(IsHouseTile(tile));
	HouseID id = GetHouseType(tile);
	const HouseSpec *hs = HouseSpec::Get(id);

	trigger_cargoes &= hs->watched_cargoes;
	/* None of the trigger cargoes is watched? */
	if (trigger_cargoes == 0) return;

	/* Same random value for all tiles of a multi-tile house. */
	uint16 r = Random();

	/* Do the callback, start at northern tile. */
	TileIndex north = tile + GetHouseNorthPart(id);
	hs = HouseSpec::Get(id);

	DoWatchedCargoCallback(north, tile, trigger_cargoes, r);
	if (hs->building_flags & BUILDING_2_TILES_Y)   DoWatchedCargoCallback(TILE_ADDXY(north, 0, 1), tile, trigger_cargoes, r);
	if (hs->building_flags & BUILDING_2_TILES_X)   DoWatchedCargoCallback(TILE_ADDXY(north, 1, 0), tile, trigger_cargoes, r);
	if (hs->building_flags & BUILDING_HAS_4_TILES) DoWatchedCargoCallback(TILE_ADDXY(north, 1, 1), tile, trigger_cargoes, r);
}

