/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file ai.hpp Base functions for all AIs. */

#ifndef AI_HPP
#define AI_HPP

#include "../script/script_scanner.hpp"
#include "../script/api/script_event_types.hpp"
#include "../string.h"
#include "../saveload/saveload_buffer.h"

/**
 * Main AI class. Contains all functions needed to start, stop, save and load AIs.
 */
class AI {
public:
	/**
	 * The default months AIs start after each other.
	 */
	enum StartNext {
		START_NEXT_EASY   = DAYS_IN_YEAR * 2,
		START_NEXT_MEDIUM = DAYS_IN_YEAR,
		START_NEXT_HARD   = DAYS_IN_YEAR / 2,
		START_NEXT_MIN    = 1,
		START_NEXT_MAX    = 3600,
		START_NEXT_DEVIATION = 60,
	};

	/**
	 * Is it possible to start a new AI company?
	 * @return True if a new AI company can be started.
	 */
	static bool CanStartNew();

	/**
	 * Start a new AI company.
	 * @param company At which slot the AI company should start.
	 * @param rerandomise_ai Whether to rerandomise the configured AI.
	 */
	static void StartNew(CompanyID company, bool rerandomise_ai = true);

	/**
	 * Called every game-tick to let AIs do something.
	 */
	static void GameLoop();

	/**
	 * Get the current AI tick.
	 */
	static uint GetTick();

	/**
	 * Stop a company to be controlled by an AI.
	 * @param company The company from which the AI needs to detach.
	 * @pre Company::IsValidAiID(company)
	 */
	static void Stop(CompanyID company);

	/**
	 * Suspend the AI and then pause execution of the script. The script
	 * will not be resumed from its suspended state until the script has
	 * been unpaused.
	 * @param company The company for which the AI should be paused.
	 * @pre Company::IsValidAiID(company)
	 */
	static void Pause(CompanyID company);

	/**
	 * Resume execution of the AI. This function will not actually execute
	 * the script, but set a flag so that the script is executed my the usual
	 * mechanism that executes the script.
	 * @param company The company for which the AI should be unpaused.
	 * @pre Company::IsValidAiID(company)
	 */
	static void Unpause(CompanyID company);

	/**
	 * Checks if the AI is paused.
	 * @param company The company for which to check if the AI is paused.
	 * @pre Company::IsValidAiID(company)
	 * @return true if the AI is paused, otherwise false.
	 */
	static bool IsPaused(CompanyID company);

	/**
	 * Kill any and all AIs we manage.
	 */
	static void KillAll();

	/**
	 * Initialize the AI system.
	 */
	static void Initialize();

	/**
	 * Uninitialize the AI system
	 * @param keepConfig Should we keep AIConfigs, or can we free that memory?
	 */
	static void Uninitialize(bool keepConfig);

	/**
	 * Reset all AIConfigs, and make them reload their AIInfo.
	 * If the AIInfo could no longer be found, an error is reported to the user.
	 */
	static void ResetConfig();

	/**
	 * Queue a new event for an AI.
	 */
	static void NewEvent(CompanyID company, ScriptEvent *event);

	/**
	 * Broadcast a new event to all active AIs.
	 */
	static void BroadcastNewEvent(ScriptEvent *event, CompanyID skip_company = MAX_COMPANIES);

	/**
	 * Save data from an AI to a savegame.
	 */
	static void Save(SaveDumper *dumper, CompanyID company);

	/**
	 * Load data for an AI from a savegame.
	 */
	static void Load(LoadBuffer *reader, CompanyID company, int version);

	/**
	 * Get the number of days before the next AI should start.
	 */
	static int GetStartNextTime();

	/** Wrapper function for AIScanner::GetAIConsoleList */
	static void GetConsoleList (stringb *buf, bool newest_only = false);
	/** Wrapper function for AIScanner::GetAIConsoleLibraryList */
	static void GetConsoleLibraryList (stringb *buf);
	/** Wrapper function for AIScanner::GetUniqueAIInfoList */
	static const ScriptInfoList::List *GetUniqueInfoList();
	/** Wrapper function for AIScanner::FindInfo */
	static class AIInfo *FindInfo(const char *name, int version, bool force_exact_match);
	/** Wrapper function for AIScanner::FindLibrary */
	static class AILibrary *FindLibrary(const char *library, int version);

	/** Check if there are no AIs available at all. */
	static bool Empty (void);

	/**
	 * Rescans all searchpaths for available AIs. If a used AI is no longer
	 * found it is removed from the config.
	 */
	static void Rescan();

#if defined(ENABLE_NETWORK)
	/** Wrapper function for AIScanner::HasAI */
	static bool HasAI(const struct ContentInfo *ci, bool md5sum);
	static bool HasAILibrary(const ContentInfo *ci, bool md5sum);

	static const char *FindInfoMainScript (const ContentInfo *ci);
	static const char *FindLibraryMainScript (const ContentInfo *ci);
#endif
private:
	static uint frame_counter;                      ///< Tick counter for the AI code
};

#endif /* AI_HPP */
