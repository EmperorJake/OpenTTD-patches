/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file null_v.cpp The video driver that doesn't blit. */

#include "../stdafx.h"
#include "../gfx_func.h"
#include "../blitter/factory.hpp"
#include "../sl/saveload.h"
#include "../window_func.h"
#include "../thread.h"
#include "null_v.h"

#include <atomic>

#include "../safeguards.h"

/** Factory for the null video driver. */
static FVideoDriver_Null iFVideoDriver_Null;

extern std::atomic<bool> _exit_game;

const char *VideoDriver_Null::Start(const StringList &parm)
{
#ifdef _MSC_VER
	/* Disable the MSVC assertion message box. */
	_set_error_mode(_OUT_TO_STDERR);
#endif

	this->UpdateAutoResolution();

	this->ticks = GetDriverParamInt(parm, "ticks", 1000);
	this->until_exit = GetDriverParamBool(parm, "until_exit");
	_screen.width  = _screen.pitch = _cur_resolution.width;
	_screen.height = _cur_resolution.height;
	_screen.dst_ptr = nullptr;
	ScreenSizeChanged();

	/* Do not render, nor blit */
	DEBUG(misc, 1, "Forcing blitter 'null'...");
	BlitterFactory::SelectBlitter("null");
	return nullptr;
}

void VideoDriver_Null::Stop() { }

void VideoDriver_Null::MakeDirty(int, int, int, int) {}

void VideoDriver_Null::MainLoop()
{
	SetSelfAsGameThread();
	if (this->until_exit) {
		while (!_exit_game) {
			::GameLoop();
			::InputLoop();
			::UpdateWindows();
		}
	} else {
		for (int i = 0; i < this->ticks; i++) {
			::GameLoop();
			::InputLoop();
			::UpdateWindows();
		}
	}

	/* If requested, make a save just before exit. The normal exit-flow is
	 * not triggered from this driver, so we have to do this manually. */
	if (_settings_client.gui.autosave_on_exit) {
		DoExitSave();
	}
}

bool VideoDriver_Null::ChangeResolution(int, int) { return false; }

bool VideoDriver_Null::ToggleFullscreen(bool) { return false; }
