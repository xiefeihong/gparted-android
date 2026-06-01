/* Copyright (C) 2024 GParted-Android Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "adb/AdbClient.h"
#include "GParted_Core.h"
#include "Utils.h"

#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>

namespace GParted
{

AdbClient::AdbClient()
	: m_device_serial("")
	, m_is_root(false)
	, m_su_available(false)
	, m_root_checked(false)
{
}

AdbClient::AdbClient(const Glib::ustring& device_serial)
	: m_device_serial(device_serial)
	, m_is_root(false)
	, m_su_available(false)
	, m_root_checked(false)
{
}

AdbClient::~AdbClient()
{
}

void AdbClient::set_device_serial(const Glib::ustring& serial)
{
	m_device_serial = serial;
}

const Glib::ustring& AdbClient::get_device_serial() const
{
	return m_device_serial;
}

bool AdbClient::is_device_connected()
{
	Glib::ustring output;
	Glib::ustring error;
	int result = Utils::execute_command(build_adb_command("devices"), output, error, true);

	if (result != 0) {
		return false;
	}

	if (m_device_serial.empty()) {
		return output.find("\tdevice") != Glib::ustring::npos;
	}

	size_t pos = output.find(m_device_serial);
	if (pos == Glib::ustring::npos) {
		return false;
	}

	pos = output.find("\tdevice", pos);
	return pos != Glib::ustring::npos;
}

bool AdbClient::wait_for_device(int timeout_seconds)
{
	for (int i = 0; i < timeout_seconds; i++) {
		if (is_device_connected()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	return false;
}

Glib::ustring AdbClient::shell_command(const Glib::ustring& command)
{
	Glib::ustring output;
	Glib::ustring error;
	shell_command_exit_status(command, output, error);
	return output + error;
}

int AdbClient::shell_command_exit_status(const Glib::ustring& command, Glib::ustring& output, Glib::ustring& error)
{
	if (!m_root_checked)
		check_root();

	Glib::ustring shell_cmd = command;
	if (!m_is_root && m_su_available)
		shell_cmd = "su -c '" + escape_single_quotes(command) + "'";

	Glib::ustring cmd = build_adb_command("shell " + shell_cmd);
	GParted_Core::log_cmd(("[AdbClient] CMD: " + cmd).c_str());
	int rc = Utils::execute_command(cmd, output, error, true);
	GParted_Core::log_out(("[AdbClient] RC=" + Utils::num_to_str(rc) + " OUTPUT=[" + output + "] ERROR=[" + error + "]").c_str());
	return rc;
}

int AdbClient::raw_shell_command_exit_status(const Glib::ustring& command, Glib::ustring& output, Glib::ustring& error)
{
	Glib::ustring cmd = build_adb_command("shell " + command);
	return Utils::execute_command(cmd, output, error, true);
}

void AdbClient::check_root()
{
	m_root_checked = true;

	Glib::ustring output, error;
	raw_shell_command_exit_status("id", output, error);
	GParted_Core::log_cmd(("[AdbClient] check_root id output: [" + output + "]").c_str());
	if (output.find("uid=0(root)") != Glib::ustring::npos)
	{
		m_is_root = true;
		m_su_available = false;
		GParted_Core::log_cmd("[AdbClient] Already running as root");
		return;
	}

	raw_shell_command_exit_status("su -c 'id'", output, error);
	GParted_Core::log_cmd(("[AdbClient] check_root su -c id output: [" + output + "]").c_str());
	if (output.find("uid=0(root)") != Glib::ustring::npos)
	{
		m_is_root = false;
		m_su_available = true;
		GParted_Core::log_cmd("[AdbClient] Root available via su");
		return;
	}

	m_is_root = false;
	m_su_available = false;
	GParted_Core::log_cmd("[AdbClient] No root access available");
}

Glib::ustring AdbClient::escape_single_quotes(const Glib::ustring& str)
{
	Glib::ustring result;
	for (size_t i = 0; i < str.size(); i++)
	{
		if (str[i] == '\'')
			result += "'\\''";
		else
			result += str[i];
	}
	return result;
}

bool AdbClient::push_file(const Glib::ustring& local_path, const Glib::ustring& remote_path)
{
	Glib::ustring output;
	Glib::ustring error;
	int result = Utils::execute_command(build_adb_command("push " + local_path + " " + remote_path), output, error, true);
	return result == 0;
}

bool AdbClient::pull_file(const Glib::ustring& remote_path, const Glib::ustring& local_path)
{
	Glib::ustring output;
	Glib::ustring error;
	int result = Utils::execute_command(build_adb_command("pull " + remote_path + " " + local_path), output, error, true);
	return result == 0;
}

bool AdbClient::is_root()
{
	if (!m_root_checked)
		check_root();
	return m_is_root;
}

bool AdbClient::remount_system()
{
	Glib::ustring output;
	shell_command("remount");
	return output.find("remounted") != Glib::ustring::npos;
}

Glib::ustring AdbClient::build_adb_command(const Glib::ustring& args) const
{
	Glib::ustring cmd = "adb";
	if (!m_device_serial.empty()) {
		cmd += " -s " + m_device_serial;
	}
	cmd += " " + args;
	return cmd;
}

} // namespace GParted