/*
 *  Copyright (C) 2020-2023  The DOSBox Staging Team
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "autoexec.h"

#include "checks.h"
#include "control.h"
#include "dosbox.h"
#include "fs_utils.h"
#include "setup.h"
#include "shell.h"
#include "string_utils.h"

#include <algorithm>
#include <iostream>
#include <sstream>

CHECK_NARROWING();

// ***************************************************************************
// Constants
// ***************************************************************************

static const std::string autoexec_file_name = "AUTOEXEC.BAT";

constexpr char char_lf = 0x0a; // line feed
constexpr char char_cr = 0x0d; // carriage return

// ***************************************************************************
// AUTOEXEC.BAT data - both source and binary
// ***************************************************************************

// Generated AUTOEXEC.BAT, un UTF-8 format
static std::string autoexec_bat_utf8 = {};
// Whether AUTOEXEC.BAT is already registered on the Z: drive
static bool is_vfile_registered = false;
// Code page used to generate Z:\AUTOEXEC.BAT from the internal UTF-8 version
static uint16_t vfile_code_page = 0;

// Data to be used to generate AUTOEXEC.BAT

// If true, put ECHO OFF before the content of [autoexec] section
static bool autoexec_has_echo_off = false;
// Environment variables to be set in AUTOEXEC.BAT
static std::map<std::string, std::string> autoexec_variables = {};

enum class Location {
	// Autogenerated commands placed BEFORE content of the [autoexec]
	GeneratedBeforeAutoexec,
	// Content of [autoexec] section from the configuration file(s)
	ConfigFileAutoexec,
	// Autogenerated commands placed AFTER content of the [autoexec]
	GeneratedAfterAutoexec,
};

// Lines to be placed in the generated AUTOEXEC.BAT, by section (location)
static std::map<Location, std::list<std::string>> autoexec_lines;

// ***************************************************************************
// AUTOEXEC.BAT generation code
// ***************************************************************************

std::string create_autoexec_bat_utf8()
{
	std::string out;

	// Helper lamdbas

	auto push_new_line = [&] { // DOS line ending is CR+LF
		out.push_back(char_cr);
		out.push_back(char_lf);
	};

	auto push_string = [&](const std::string& line) {
		if (line.empty()) {
			push_new_line();
			return;
		}

		for (const auto& character : line) {
			out.push_back(character);
		}
		push_new_line();
	};

	// Generate AUTOEXEC.BAT, in UTF-8 format

	// If currently printed lines are autogenerated
	bool prints_generated = false;
	// If currently printed lines are from [autoexec] section of config file
	bool prints_config_section = false;

	static const std::string comment = ":: ";
	static const std::string comment_generated =
	        comment + MSG_Get("AUTOEXEC_BAT_AUTOGENERATED");
	static const std::string comment_config_section =
	        comment + MSG_Get("AUTOEXEC_BAT_CONFIG_SECTION");

	// Put 'ECHO OFF' and 'SET variable=value' if needed

	if (autoexec_has_echo_off || !autoexec_variables.empty()) {
		push_string(comment_generated);
		prints_generated = true;
	}

	if (autoexec_has_echo_off) {
		push_new_line();
		push_string("@ECHO OFF");
	}

	if (!autoexec_variables.empty()) {
		push_new_line();
		for (const auto& [name, value] : autoexec_variables) {
			push_string("@SET " + name + "=" + value);
		}
	}

	if (prints_generated) {
		push_new_line();
	}

	// Put remaining AUTOEXEC.BAT content

	for (const auto& [source, list_lines] : autoexec_lines) {
		if (list_lines.empty()) {
			continue;
		}

		switch (source) {
		case Location::GeneratedBeforeAutoexec:
		case Location::GeneratedAfterAutoexec:
			if (!prints_generated) {
				if (!out.empty()) {
					push_new_line();
				}
				push_string(comment_generated);
				push_new_line();
				prints_generated      = true;
				prints_config_section = false;
			}
			break;
		case Location::ConfigFileAutoexec:
			if (!prints_config_section) {
				if (!out.empty()) {
					push_new_line();
				}
				push_string(comment_config_section);
				push_new_line();
				prints_generated      = false;
				prints_config_section = true;
			}
			break;
		default: assert(false);
		}

		for (const auto& autoexec_line : list_lines) {
			push_string(autoexec_line);
		}
	}

	return out;
}

static void create_autoexec_bat_dos(const std::string& input_utf8,
                                    const uint16_t code_page)
{
	// Convert UTF-8 AUTOEXEC.BAT to DOS code page
	std::string autoexec_bat_dos = {};
	utf8_to_dos(input_utf8, autoexec_bat_dos, code_page);

	// Convert the result to a binary format
	auto autoexec_bat_bin = std::vector<uint8_t>(autoexec_bat_dos.begin(),
	                                             autoexec_bat_dos.end());

	// Register/refresh Z:\AUTOEXEC.BAT file
	if (is_vfile_registered) {
		VFILE_Update(autoexec_file_name.c_str(), std::move(autoexec_bat_bin));
	} else {
		VFILE_Register(autoexec_file_name.c_str(),
		               std::move(autoexec_bat_bin));
		is_vfile_registered = true;
	}

	// Store current code page for caching purposes
	vfile_code_page = code_page;
}

// ***************************************************************************
// AUTOEXEC class declaration and implementation
// ***************************************************************************

class AutoExecModule final : public Module_base {
public:
	AutoExecModule(Section* configuration);

private:
	void ProcessConfigFile(const Section_line& section,
	                       const std::string& source_name);

	void AutoMountDrive(const std::string& dir_letter);

	void AddCommandBefore(const std::string& line);
	void AddCommandAfter(const std::string& line);
	void AddAutoExecLine(const std::string& line);

	void AddMessages();
};

AutoExecModule::AutoExecModule(Section* configuration)
        : Module_base(configuration)
{
	AddMessages();

	// Get the [dosbox] conf section
	const auto sec = static_cast<Section_prop*>(control->GetSection("dosbox"));
	assert(sec);

	// Auto-mount drives (except for DOSBox's Z:) prior to [autoexec]
	if (sec->Get_bool("automount")) {
		for (char letter = 'a'; letter <= 'z'; ++letter) {
			AutoMountDrive({letter});
		}
	}

	// Initialize configurable states that control misc behavior

	// Check -securemode switch to disable mount/imgmount/boot after
	// running autoexec.bat
	const auto cmdline = control->cmdline; // short-lived copy
	const bool secure  = cmdline->FindExist("-securemode", true);

	// Are autoexec sections permitted?
	const bool autoexec_is_allowed = !cmdline->FindExist("-noautoexec", true);

	// Should autoexec sections be joined or overwritten?
	const std::string_view section_pref = sec->Get_string("autoexec_section");
	const bool should_join_autoexecs = (section_pref == "join");

	// Check to see for extra command line options to be added
	// (before the command specified on commandline)
	std::string argument = {};

	bool exit_call_exists = false;
	while (cmdline->FindString("-c", argument, true)) {
#if defined(WIN32)
		// Replace single with double quotes so that mount commands
		// can contain spaces
		for (auto& character : argument) {
			if (character == '\'') {
				character = '\"';
			}
		}
#endif // Linux users can simply use \" in their shell

		// If the user's added an exit call, simply store that
		// fact but don't insert it because otherwise it can
		// precede follow on [autoexec] calls.
		if (argument == "exit" || argument == "\"exit\"") {
			exit_call_exists = true;
			continue;
		}
		AddCommandBefore(argument);
	}

	// Check for the -exit switch, which indicates they want to quit
	const bool exit_arg_exists = cmdline->FindExist("-exit");

	// Check if instant-launch is active
	const bool using_instant_launch_with_executable =
	        control->GetStartupVerbosity() == Verbosity::InstantLaunch &&
	        cmdline->HasExecutableName();

	// Should we add an 'exit' call to the end of autoexec.bat?
	const bool should_add_exit = exit_call_exists || exit_arg_exists ||
	                             using_instant_launch_with_executable;

	auto maybe_add_command_secure = [&](const bool after = false) {
		const std::string command = "@Z:\\CONFIG.COM -securemode";
		if (secure) {
			if (after) {
				AddCommandAfter(command);
			} else {
				AddCommandBefore(command);
			}
		}
	};

	auto maybe_add_command_mount_d_cdrom = [&](const std::string& targets) {
		if (targets.empty()) {
			return;
		}
		AddCommandBefore(std::string("@Z:\\IMGMOUNT.COM D ") + targets +
		                 std::string(" -t iso"));
	};

	auto add_command_mount_c_directory = [&](const std::string& target) {
		AddCommandBefore(std::string("@Z:\\MOUNT.COM C ") + target);
		AddCommandBefore("@C:");
	};

	// Check for first argument being a directory or file

	const std::string quote = "\"";

	unsigned int index = 1;
	bool found_dir_or_command = false;
	std::string cdrom_images  = {};

	while (cmdline->FindCommand(index++, argument)) {
		// Check if argument is a file/directory

		std_fs::path path = argument;
		bool is_directory = std_fs::is_directory(path);
		if (!is_directory) {
			path = std_fs::current_path() / path;
			is_directory = std_fs::is_directory(path);
		}

		if (is_directory) {
			maybe_add_command_mount_d_cdrom(cdrom_images);
			add_command_mount_c_directory(quote + argument + quote);
			maybe_add_command_secure();

			found_dir_or_command = true;
			break;
		}

		// Check if argument is a batch file

		auto argument_ucase = argument;
		upcase(argument_ucase);
		if (ends_with(argument_ucase, ".BAT")) {
			maybe_add_command_mount_d_cdrom(cdrom_images);
			maybe_add_command_secure();
			// BATch files are called else exit will not work
			AddCommandBefore(std::string("CALL ") + argument);

			found_dir_or_command = true;
			break;
		}

		// Check if argument is a boot image file

		if (ends_with(argument_ucase, ".IMG") ||
		    ends_with(argument_ucase, ".IMA")) {
			maybe_add_command_mount_d_cdrom(cdrom_images);
			// No secure mode here as boot is destructive and
			// enabling securemode disables boot
			AddCommandBefore(std::string("BOOT ") + quote +
			                 argument + quote);

			found_dir_or_command = true;
			break;
		}

		// Check if argument is a CD image

		if (ends_with(argument_ucase, ".ISO") ||
		    ends_with(argument_ucase, ".CUE")) {
			if (!cdrom_images.empty()) {
				cdrom_images += " ";
			}
			cdrom_images += quote + argument + quote;
			continue;
		}

		// Consider argument as a command

		maybe_add_command_mount_d_cdrom(cdrom_images);
		maybe_add_command_secure();
		AddCommandBefore(argument);

		found_dir_or_command = true;
		break;
	}

	// Generate AUTOEXEC.BAT

	if (autoexec_is_allowed) {
		if (should_join_autoexecs) {
			ProcessConfigFile(*static_cast<const Section_line*>(configuration),
			                  "one or more joined sections");
		} else if (found_dir_or_command) {
			LOG_MSG("AUTOEXEC: Using commands provided on the command line");
		} else {
			ProcessConfigFile(control->GetOverwrittenAutoexecSection(),
			                  control->GetOverwrittenAutoexecConf());
		}
	}

	if (!found_dir_or_command) {
		maybe_add_command_mount_d_cdrom(cdrom_images);
		// If we're in secure mode without command line
		// executables, then seal off the configuration
		constexpr bool after = true;
		maybe_add_command_secure(after);
	}

	if (should_add_exit) {
		AddCommandAfter("@EXIT");
	}

	// Register the AUTOEXEC.BAT file if not already done
	AUTOEXEC_RegisterFile();
}

void AutoExecModule::ProcessConfigFile(const Section_line& section,
                                       const std::string& source_name)
{
	if (section.data.empty()) {
		return;
	}

	LOG_MSG("AUTOEXEC: Using autoexec from %s", source_name.c_str());

	auto check_echo_off = [](const std::string& line) {
		if (line.empty()) {
			return false;
		}

		std::string tmp = (line[0] == '@') ? line.substr(1) : line;
		if (tmp.length() < 8) {
			return false;
		}

		lowcase(tmp);
		if (tmp.substr(0, 4) != "echo" || !ends_with(tmp, "off")) {
			return false;
		}

		tmp = tmp.substr(4, tmp.length() - 7);
		for (const auto character : tmp) {
			if (!isspace(character)) {
				return false;
			}
		}

		return true;
	};

	std::istringstream input;
	input.str(section.data);

	std::string line = {};

	bool is_first_line = true;
	while (std::getline(input, line)) {
		trim(line);

		// If the first line is 'echo off' command, skip it and replace
		// with auto-generated one

		if (is_first_line) {
			is_first_line = false;
			if (check_echo_off(line)) {
				autoexec_has_echo_off = true;
				continue;
			}
		}

		AddAutoExecLine(line);
	}
}

// Takes in a drive letter (eg: 'c') and attempts to mount the 'drives/c',
// extends system path if needed
void AutoExecModule::AutoMountDrive(const std::string& dir_letter)
{
	// Does drives/[x] exist?
	const auto drive_path = GetResourcePath("drives", dir_letter);
	if (!path_exists(drive_path)) {
		return;
	}

	// Try parsing the [x].conf file
	const auto conf_path  = drive_path.string() + ".conf";
	const auto [drive_letter, mount_args, path_val] =
		parse_drive_conf(dir_letter, conf_path);

	// Install mount as an autoexec command
	AddCommandBefore(std::string("@Z:\\MOUNT.COM ") + drive_letter + " \"" +
	                 simplify_path(drive_path).string() + "\"" + mount_args);

	// Install path as an autoexec command
	if (path_val.length()) {
		AddCommandBefore(std::string("@SET PATH=") + path_val);
	}
}

void AutoExecModule::AddCommandBefore(const std::string& line)
{
	autoexec_lines[Location::GeneratedBeforeAutoexec].push_back(line);
}

void AutoExecModule::AddCommandAfter(const std::string& line)
{
	autoexec_lines[Location::GeneratedAfterAutoexec].push_back(line);
}

void AutoExecModule::AddAutoExecLine(const std::string& line)
{
	autoexec_lines[Location::ConfigFileAutoexec].push_back(line);
}

void AutoExecModule::AddMessages()
{
	MSG_Add("AUTOEXEC_BAT_AUTOGENERATED", "autogenerated");
	MSG_Add("AUTOEXEC_BAT_CONFIG_SECTION", "from [autoexec] section");
}

void AUTOEXEC_NotifyNewCodePage()
{
	// No need to do anything during the shutdown or if Z:\AUTOEXEC.BAT file
	// does not exist yet
	if (shutdown_requested || !is_vfile_registered) {
		return;
	}

	// No need to do anything if the code page used by UTF-8 engine is still
	// the same as when Z:\AUTOEXEC.BAT was generated/refreshed
	const auto code_page = get_utf8_code_page();
	if (code_page == vfile_code_page) {
		return;
	}

	// Recreate the AUTOEXEC.BAT file as visible on DOS side
	create_autoexec_bat_dos(autoexec_bat_utf8, code_page);
}

void AUTOEXEC_SetVariable(const std::string& name, const std::string& value)
{
#if C_DEBUG
	if (!std::all_of(name.cbegin(), name.cend(), is_printable_ascii)) {
		E_Exit("AUTOEXEC: Variable name is not a printable ASCII");
	}
	if (!std::all_of(value.cbegin(), value.cend(), is_printable_ascii)) {
		E_Exit("AUTOEXEC: Variable value is not a printable ASCII");
	}
#endif

	auto name_upcase = name;
	upcase(name_upcase);

	// If shell is already running, refresh variable content
	if (first_shell) {
		first_shell->SetEnv(name_upcase.c_str(), value.c_str());
	}

	// Update our internal list of variables to set in AUTOEXEC.BAT
	if (value.empty()) {
		autoexec_variables.erase(name_upcase);
	} else {
		autoexec_variables[name_upcase] = value;
	}
}

void AUTOEXEC_RegisterFile()
{
	autoexec_bat_utf8 = create_autoexec_bat_utf8();
	create_autoexec_bat_dos(autoexec_bat_utf8, get_utf8_code_page());
}

static std::unique_ptr<AutoExecModule> autoexec_module{};

void AUTOEXEC_Init(Section* sec)
{
	autoexec_module = std::make_unique<AutoExecModule>(sec);
}
