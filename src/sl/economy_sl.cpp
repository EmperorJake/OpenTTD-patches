/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file economy_sl.cpp Code handling saving and loading of economy data */

#include "../stdafx.h"
#include "../economy_func.h"
#include "../economy_base.h"

#include "saveload.h"

#include "../safeguards.h"

/** Prices in pre 126 savegames */
static void Load_PRIC()
{
	/* Old games store 49 base prices, very old games store them as int32_t */
	int vt = IsSavegameVersionBefore(SLV_65) ? SLE_FILE_I32 : SLE_FILE_I64;
	SlArray(nullptr, 49, vt | SLE_VAR_NULL);
	SlArray(nullptr, 49, SLE_FILE_U16 | SLE_VAR_NULL);
}

/** Cargo payment rates in pre 126 savegames */
static void Load_CAPR()
{
	uint num_cargo = IsSavegameVersionBefore(SLV_55) ? 12 : IsSavegameVersionBefore(SLV_EXTEND_CARGOTYPES) ? 32 : NUM_CARGO;
	int vt = IsSavegameVersionBefore(SLV_65) ? SLE_FILE_I32 : SLE_FILE_I64;
	SlArray(nullptr, num_cargo, vt | SLE_VAR_NULL);
	SlArray(nullptr, num_cargo, SLE_FILE_U16 | SLE_VAR_NULL);
}

static const SaveLoad _economy_desc[] = {
	SLE_CONDNULL(4,                                                                  SL_MIN_VERSION, SLV_65),             // max_loan
	SLE_CONDNULL(8,                                                                 SLV_65, SLV_144), // max_loan
	SLE_CONDVAR(Economy, old_max_loan_unround,          SLE_FILE_I32 | SLE_VAR_I64,  SL_MIN_VERSION, SLV_65),
	SLE_CONDVAR(Economy, old_max_loan_unround,          SLE_INT64,                  SLV_65, SLV_126),
	SLE_CONDVAR(Economy, old_max_loan_unround_fract,    SLE_UINT16,                 SLV_70, SLV_126),
	SLE_CONDVAR(Economy, inflation_prices,              SLE_UINT64,                SLV_126, SL_MAX_VERSION),
	SLE_CONDVAR(Economy, inflation_payment,             SLE_UINT64,                SLV_126, SL_MAX_VERSION),
	    SLE_VAR(Economy, fluct,                         SLE_INT16),
	    SLE_VAR(Economy, interest_rate,                 SLE_UINT8),
	    SLE_VAR(Economy, infl_amount,                   SLE_UINT8),
	    SLE_VAR(Economy, infl_amount_pr,                SLE_UINT8),
	SLE_CONDVAR(Economy, industry_daily_change_counter, SLE_UINT32,                SLV_102, SL_MAX_VERSION),
	SLE_CONDNULL_X(8, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_JOKERPP)),
};

/** Economy variables */
static void Save_ECMY()
{
	SlObject(&_economy, _economy_desc);
}

/** Economy variables */
static void Load_ECMY()
{
	SlObject(&_economy, _economy_desc);
	StartupIndustryDailyChanges(IsSavegameVersionBefore(SLV_102));  // old savegames will need to be initialized
}

static const SaveLoad _cargopayment_desc[] = {
	    SLE_REF(CargoPayment, front,           REF_VEHICLE),
	    SLE_VAR(CargoPayment, route_profit,    SLE_INT64),
	    SLE_VAR(CargoPayment, visual_profit,   SLE_INT64),
	SLE_CONDVAR_X(CargoPayment, visual_transfer, SLE_INT64, SLV_181, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_OR, XSLFI_CHILLPP)),
};

static void Save_CAPY()
{
	for (CargoPayment *cp : CargoPayment::Iterate()) {
		SlSetArrayIndex(cp->index);
		SlObject(cp, _cargopayment_desc);
	}
}

static void Load_CAPY()
{
	int index;

	while ((index = SlIterateArray()) != -1) {
		CargoPayment *cp = new (index) CargoPayment();
		SlObject(cp, _cargopayment_desc);
	}
}

static void Ptrs_CAPY()
{
	for (CargoPayment *cp : CargoPayment::Iterate()) {
		SlObject(cp, _cargopayment_desc);
	}
}


static const ChunkHandler economy_chunk_handlers[] = {
	{ 'CAPY', Save_CAPY, Load_CAPY, Ptrs_CAPY, nullptr, CH_ARRAY },
	{ 'PRIC', nullptr,   Load_PRIC, nullptr,   nullptr, CH_RIFF  },
	{ 'CAPR', nullptr,   Load_CAPR, nullptr,   nullptr, CH_RIFF  },
	{ 'ECMY', Save_ECMY, Load_ECMY, nullptr,   nullptr, CH_RIFF  },
};

extern const ChunkHandlerTable _economy_chunk_handlers(economy_chunk_handlers);
