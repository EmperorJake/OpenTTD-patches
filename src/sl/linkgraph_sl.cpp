/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file linkgraph_sl.cpp Code handling saving and loading of link graphs */

#include "../stdafx.h"
#include "../linkgraph/linkgraph.h"
#include "../linkgraph/linkgraphjob.h"
#include "../linkgraph/linkgraphschedule.h"
#include "../network/network.h"
#include "../settings_internal.h"
#include "saveload.h"

#include "../safeguards.h"

typedef LinkGraph::BaseNode Node;
typedef LinkGraph::BaseEdge Edge;

static uint16_t _num_nodes;

/**
 * Get a SaveLoad array for a link graph.
 * @return SaveLoad array for link graph.
 */
SaveLoadTable GetLinkGraphDesc()
{
	static const SaveLoad link_graph_desc[] = {
	SLE_CONDVAR_X(LinkGraph, last_compression, SLE_VAR_I64 | SLE_FILE_I32, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_LINKGRAPH_DAY_SCALE, 0, 3)),
	SLE_CONDVAR_X(LinkGraph, last_compression,                  SLE_INT64,  SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_LINKGRAPH_DAY_SCALE, 4)),
		SLEG_VAR(_num_nodes,                  SLE_UINT16),
		 SLE_VAR(LinkGraph, cargo,            SLE_UINT8),
	};
	return link_graph_desc;
}

void GetLinkGraphJobDayLengthScaleAfterLoad(LinkGraphJob *lgj)
{
	lgj->join_date_ticks.edit_base() *= DAY_TICKS;
	lgj->join_date_ticks += LinkGraphSchedule::SPAWN_JOIN_TICK;

	uint recalc_scale;
	if (IsSavegameVersionBefore(SLV_LINKGRAPH_SECONDS) && SlXvIsFeatureMissing(XSLFI_LINKGRAPH_DAY_SCALE, 3)) {
		/* recalc time is in days */
		recalc_scale = DAY_TICKS;
	} else {
		/* recalc time is in seconds */
		recalc_scale = DAY_TICKS / SECONDS_PER_DAY;
	}
	lgj->start_date_ticks = lgj->join_date_ticks - (lgj->Settings().recalc_time * recalc_scale);
}

/**
 * Get a SaveLoad array for a link graph job. The settings struct is derived from
 * the global settings saveload array. The exact entries are calculated when the function
 * is called the first time.
 * It's necessary to keep a copy of the settings for each link graph job so that you can
 * change the settings while in-game and still not mess with current link graph runs.
 * Of course the settings have to be saved and loaded, too, to avoid desyncs.
 * @return Array of SaveLoad structs.
 */
SaveLoadTable GetLinkGraphJobDesc()
{
	static std::vector<SaveLoad> saveloads;

	/* Build the SaveLoad array on first call and don't touch it later on */
	if (saveloads.size() == 0) {
		size_t offset_gamesettings = cpp_offsetof(GameSettings, linkgraph);
		size_t offset_component = cpp_offsetof(LinkGraphJob, settings);

		const SettingTable &linkgraph_table = GetLinkGraphSettingTable();
		for (const auto &desc : linkgraph_table) {
			SaveLoad sl = desc->save;
			if (GetVarMemType(sl.conv) != SLE_VAR_NULL) {
				char *&address = reinterpret_cast<char *&>(sl.address);
				address -= offset_gamesettings;
				address += offset_component;
			}
			saveloads.push_back(sl);
		}

		const SaveLoad job_desc[] = {
			SLE_CONDVAR_X(LinkGraphJob, join_date_ticks,  SLE_FILE_I32 | SLE_VAR_I64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_LINKGRAPH_DAY_SCALE, 0, 4)),
			SLE_CONDVAR_X(LinkGraphJob, join_date_ticks,  SLE_INT64,                  SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_LINKGRAPH_DAY_SCALE, 5)),
			SLE_CONDVAR_X(LinkGraphJob, start_date_ticks, SLE_FILE_I32 | SLE_VAR_I64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_LINKGRAPH_DAY_SCALE, 1, 4)),
			SLE_CONDVAR_X(LinkGraphJob, start_date_ticks, SLE_INT64,                  SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_LINKGRAPH_DAY_SCALE, 5)),
			SLE_VAR(LinkGraphJob, link_graph.index, SLE_UINT16),
		};

		for (auto &sld : job_desc) {
			saveloads.push_back(sld);
		}
	}

	return saveloads;
}

/**
 * Get a SaveLoad array for the link graph schedule.
 * @return SaveLoad array for the link graph schedule.
 */
SaveLoadTable GetLinkGraphScheduleDesc()
{
	static const SaveLoad schedule_desc[] = {
		SLE_REFLIST(LinkGraphSchedule, schedule, REF_LINK_GRAPH),
		SLE_REFLIST(LinkGraphSchedule, running,  REF_LINK_GRAPH_JOB),
	};
	return schedule_desc;
}

/* Edges and nodes are saved in the correct order, so we don't need to save their IDs. */

/**
 * SaveLoad desc for a link graph node.
 */
static const SaveLoad _node_desc[] = {
	SLE_CONDVAR(Node, xy,          SLE_UINT32, SLV_191, SL_MAX_VERSION),
	    SLE_VAR(Node, supply,      SLE_UINT32),
	    SLE_VAR(Node, demand,      SLE_UINT32),
	    SLE_VAR(Node, station,     SLE_UINT16),
	    SLE_VAR(Node, last_update, SLE_INT32),
};

/**
 * SaveLoad desc for a link graph edge.
 */
static const SaveLoad _edge_desc[] = {
	SLE_CONDNULL(4, SL_MIN_VERSION, SLV_191), // distance
	     SLE_VAR(Edge, capacity,                 SLE_UINT32),
	     SLE_VAR(Edge, usage,                    SLE_UINT32),
	SLE_CONDVAR_X(Edge, travel_time_sum,         SLE_UINT64, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_LINKGRAPH_TRAVEL_TIME)),
	     SLE_VAR(Edge, last_unrestricted_update, SLE_INT32),
	 SLE_CONDVAR(Edge, last_restricted_update,   SLE_INT32, SLV_187, SL_MAX_VERSION),
	SLE_CONDVAR_X(Edge, last_aircraft_update,    SLE_INT32, SL_MIN_VERSION, SL_MAX_VERSION, SlXvFeatureTest(XSLFTO_AND, XSLFI_LINKGRAPH_AIRCRAFT)),
//	     SLE_VAR(Edge, next_edge,                SLE_UINT16), // Removed since XSLFI_LINKGRAPH_SPARSE_EDGES
};

std::vector<SaveLoad> _filtered_node_desc;
std::vector<SaveLoad> _filtered_edge_desc;
std::vector<SaveLoad> _filtered_job_desc;

static void FilterDescs()
{
	_filtered_node_desc = SlFilterObject(_node_desc);
	_filtered_edge_desc = SlFilterObject(_edge_desc);
	_filtered_job_desc = SlFilterObject(GetLinkGraphJobDesc());
}

/**
 * Save a link graph.
 * @param lg Link graph to be saved or loaded.
 */
void Save_LinkGraph(LinkGraph &lg)
{
	uint16_t size = lg.Size();
	auto edge_iter = lg.edges.begin();
	auto edge_end = lg.edges.end();
	for (NodeID from = 0; from < size; ++from) {
		Node *node = &lg.nodes[from];
		SlObjectSaveFiltered(node, _filtered_node_desc);

		while (edge_iter != edge_end && edge_iter->first.first == from) {
			SlWriteUint16(edge_iter->first.second);
			Edge *edge = &edge_iter->second;
			SlObjectSaveFiltered(edge, _filtered_edge_desc);
			++edge_iter;
		}
		SlWriteUint16(INVALID_NODE);
	}
}

/**
 * Load a link graph.
 * @param lg Link graph to be saved or loaded.
 */
void Load_LinkGraph(LinkGraph &lg)
{
	uint size = lg.Size();
	if (SlXvIsFeaturePresent(XSLFI_LINKGRAPH_SPARSE_EDGES)) {
		for (NodeID from = 0; from < size; ++from) {
			Node *node = &lg.nodes[from];
			SlObjectLoadFiltered(node, _filtered_node_desc);
			while (true) {
				NodeID to = SlReadUint16();
				if (to == INVALID_NODE) break;
				SlObjectLoadFiltered(&lg.edges[std::make_pair(from, to)], _filtered_edge_desc);
			}
		}
	} else if (IsSavegameVersionBefore(SLV_191)) {
		std::vector<Edge> temp_edges;
		std::vector<NodeID> temp_next_edges;
		temp_edges.resize(size);
		temp_next_edges.resize(size);
		for (NodeID from = 0; from < size; ++from) {
			Node *node = &lg.nodes[from];
			SlObjectLoadFiltered(node, _filtered_node_desc);
			/* We used to save the full matrix ... */
			for (NodeID to = 0; to < size; ++to) {
				SlObjectLoadFiltered(&temp_edges[to], _filtered_edge_desc);
				temp_next_edges[to] = SlReadUint16();
			}
			for (NodeID to = from; to != INVALID_NODE; to = temp_next_edges[to]) {
				lg.edges[std::make_pair(from, to)] = temp_edges[to];
			}
		}
	} else {
		for (NodeID from = 0; from < size; ++from) {
			Node *node = &lg.nodes[from];
			SlObjectLoadFiltered(node, _filtered_node_desc);
			/* ... but as that wasted a lot of space we save a sparse matrix now. */
			for (NodeID to = from; to != INVALID_NODE;) {
				if (to >= size) SlErrorCorrupt("Link graph structure overflow");
				SlObjectLoadFiltered(&lg.edges[std::make_pair(from, to)], _filtered_edge_desc);
				to = SlReadUint16();
			}
		}
	}
}

/**
 * Save a link graph job.
 * @param lgj LinkGraphJob to be saved.
 */
static void DoSave_LGRJ(LinkGraphJob *lgj)
{
	SlObjectSaveFiltered(lgj, _filtered_job_desc);
	_num_nodes = lgj->Size();
	SlObject(const_cast<LinkGraph *>(&lgj->Graph()), GetLinkGraphDesc());
	Save_LinkGraph(const_cast<LinkGraph &>(lgj->Graph()));
}

/**
 * Save a link graph.
 * @param lg LinkGraph to be saved.
 */
static void DoSave_LGRP(LinkGraph *lg)
{
	_num_nodes = lg->Size();
	SlObject(lg, GetLinkGraphDesc());
	Save_LinkGraph(*lg);
}

/**
 * Load all link graphs.
 */
static void Load_LGRP()
{
	FilterDescs();
	int index;
	while ((index = SlIterateArray()) != -1) {
		if (!LinkGraph::CanAllocateItem()) {
			/* Impossible as they have been present in previous game. */
			NOT_REACHED();
		}
		LinkGraph *lg = new (index) LinkGraph();
		SlObject(lg, GetLinkGraphDesc());
		lg->Init(_num_nodes);
		Load_LinkGraph(*lg);
	}
}

/**
 * Load all link graph jobs.
 */
static void Load_LGRJ()
{
	FilterDescs();
	int index;
	while ((index = SlIterateArray()) != -1) {
		if (!LinkGraphJob::CanAllocateItem()) {
			/* Impossible as they have been present in previous game. */
			NOT_REACHED();
		}
		LinkGraphJob *lgj = new (index) LinkGraphJob();
		SlObjectLoadFiltered(lgj, _filtered_job_desc);
		if (SlXvIsFeatureMissing(XSLFI_LINKGRAPH_DAY_SCALE)) {
			extern void GetLinkGraphJobDayLengthScaleAfterLoad(LinkGraphJob *lgj);
			GetLinkGraphJobDayLengthScaleAfterLoad(lgj);
		}
		LinkGraph &lg = const_cast<LinkGraph &>(lgj->Graph());
		SlObject(&lg, GetLinkGraphDesc());
		lg.Init(_num_nodes);
		Load_LinkGraph(lg);
	}
}

/**
 * Load the link graph schedule.
 */
static void Load_LGRS()
{
	SlObject(&LinkGraphSchedule::instance, GetLinkGraphScheduleDesc());
}

/**
 * Spawn the threads for running link graph calculations.
 * Has to be done after loading as the cargo classes might have changed.
 */
void AfterLoadLinkGraphs()
{
	if (IsSavegameVersionBefore(SLV_191)) {
		for (LinkGraph *lg : LinkGraph::Iterate()) {
			for (NodeID node_id = 0; node_id < lg->Size(); ++node_id) {
				const Station *st = Station::GetIfValid((*lg)[node_id].Station());
				if (st != nullptr) (*lg)[node_id].UpdateLocation(st->xy);
			}
		}

		for (LinkGraphJob *lgj : LinkGraphJob::Iterate()) {
			LinkGraph *lg = &(const_cast<LinkGraph &>(lgj->Graph()));
			for (NodeID node_id = 0; node_id < lg->Size(); ++node_id) {
				const Station *st = Station::GetIfValid((*lg)[node_id].Station());
				if (st != nullptr) (*lg)[node_id].UpdateLocation(st->xy);
			}
		}
	}

	LinkGraphSchedule::instance.SpawnAll();

	if (!_networking || _network_server) {
		AfterLoad_LinkGraphPauseControl();
	}
}

/**
 * Save all link graphs.
 */
static void Save_LGRP()
{
	FilterDescs();
	for (LinkGraph *lg : LinkGraph::Iterate()) {
		SlSetArrayIndex(lg->index);
		SlAutolength((AutolengthProc*)DoSave_LGRP, lg);
	}
}

/**
 * Save all link graph jobs.
 */
static void Save_LGRJ()
{
	FilterDescs();
	for (LinkGraphJob *lgj : LinkGraphJob::Iterate()) {
		SlSetArrayIndex(lgj->index);
		SlAutolength((AutolengthProc*)DoSave_LGRJ, lgj);
	}
}

/**
 * Save the link graph schedule.
 */
static void Save_LGRS()
{
	SlObject(&LinkGraphSchedule::instance, GetLinkGraphScheduleDesc());
}

/**
 * Substitute pointers in link graph schedule.
 */
static void Ptrs_LGRS()
{
	SlObject(&LinkGraphSchedule::instance, GetLinkGraphScheduleDesc());
}

static const ChunkHandler linkgraph_chunk_handlers[] = {
	{ 'LGRP', Save_LGRP, Load_LGRP, nullptr,   nullptr, CH_ARRAY },
	{ 'LGRJ', Save_LGRJ, Load_LGRJ, nullptr,   nullptr, CH_ARRAY },
	{ 'LGRS', Save_LGRS, Load_LGRS, Ptrs_LGRS, nullptr, CH_RIFF  }
};

extern const ChunkHandlerTable _linkgraph_chunk_handlers(linkgraph_chunk_handlers);
