#include "adb/PartedCommand.h"
#include "Utils.h"

#include <glibmm/ustring.h>
#include <sstream>
#include <cctype>
#include <clocale>

namespace GParted
{

PartedCommand::PartedCommand(AdbClient& adb) : m_adb(adb) {}

Glib::ustring PartedCommand::list_devices()
{
	return print_all("s");
}

static bool is_disk_device(const Glib::ustring& name)
{
	if (name.empty()) return false;

	if (name.find("loop") == 0) return false;
	if (name.find("dm-") == 0) return false;
	if (name.find("ram") == 0) return false;
	if (name.find("zram") == 0) return false;

	bool is_sd = (name.find("sd") == 0);
	bool is_mmc = (name.find("mmcblk") == 0);
	bool is_nvme = (name.find("nvme") == 0);
	bool is_vd = (name.find("vd") == 0);
	bool is_hd = (name.find("hd") == 0);
	if (!is_sd && !is_mmc && !is_nvme && !is_vd && !is_hd)
		return false;

	size_t p_pos = name.find('p');
	if (p_pos != Glib::ustring::npos && p_pos > 0 && std::isdigit(name[p_pos - 1]))
	{
		if (p_pos + 1 < name.size() && std::isdigit(name[p_pos + 1]))
			return false;
	}

	if (!name.empty() && std::isdigit(name[name.size() - 1]))
	{
		if (is_mmc) return true;
		if (is_nvme && name.find('p') == Glib::ustring::npos) return true;
		return false;
	}

	return true;
}

Glib::ustring PartedCommand::print_device(const Glib::ustring& device_path, const Glib::ustring& unit)
{
	return parted_cmd(device_path + " unit " + unit + " print");
}

Glib::ustring PartedCommand::print_all(const Glib::ustring& unit)
{
	Glib::ustring result;
	Glib::ustring error;

	m_adb.shell_command_exit_status("ls -1 /dev/block/", result, error);

	std::stringstream ss(result.raw());
	std::string line;
	Glib::ustring combined;
	bool found = false;

	while (std::getline(ss, line))
	{
		Glib::ustring name = Utils::trim(Glib::ustring(line));
		if (name.empty()) continue;
		if (!is_disk_device(name)) continue;

		Glib::ustring full_path = "/dev/block/" + name;
		Glib::ustring output = m_adb.shell_command(
			"parted -s " + full_path + " unit " + unit + " print");

		if (output.find("Disk ") != Glib::ustring::npos)
		{
			if (found) combined += "\n";
			combined += output;
			found = true;
		}
	}

	return combined;
}

bool PartedCommand::mklabel(const Glib::ustring& device_path, const Glib::ustring& label_type)
{
	Glib::ustring output = parted_cmd(device_path + " mklabel " + label_type);

	if (output.find("Error:") != Glib::ustring::npos)
	{
		m_last_error = output;
		return false;
	}
	return true;
}

bool PartedCommand::mkpart(const Glib::ustring& device_path, const Glib::ustring& part_type,
                           const Glib::ustring& fs_type, Sector start, Sector end)
{
	std::stringstream ss;
	ss << device_path << " mkpart " << part_type << " " << fs_type << " " << start << "s " << end << "s";

	Glib::ustring output = parted_cmd(ss.str());

	if (output.find("Error:") != Glib::ustring::npos)
	{
		m_last_error = output;
		return false;
	}
	return true;
}

bool PartedCommand::rm(const Glib::ustring& device_path, int partition_number)
{
	std::stringstream ss;
	ss << device_path << " rm " << partition_number;

	Glib::ustring output = parted_cmd(ss.str());

	if (output.find("Error:") != Glib::ustring::npos)
	{
		m_last_error = output;
		return false;
	}
	return true;
}

bool PartedCommand::resizepart(const Glib::ustring& device_path, int partition_number, Sector end)
{
	std::stringstream ss;
	ss << device_path << " resizepart " << partition_number << " " << end << "s";

	Glib::ustring output = parted_cmd(ss.str());

	if (output.find("Error:") != Glib::ustring::npos)
	{
		m_last_error = output;
		return false;
	}
	return true;
}

bool PartedCommand::set_flag(const Glib::ustring& device_path, int partition_number,
                             const Glib::ustring& flag, bool state)
{
	std::stringstream ss;
	ss << device_path << " set " << partition_number << " " << flag << " " << (state ? "on" : "off");

	Glib::ustring output = parted_cmd(ss.str());

	if (output.find("Error:") != Glib::ustring::npos)
	{
		m_last_error = output;
		return false;
	}
	return true;
}

bool PartedCommand::name_partition(const Glib::ustring& device_path, int partition_number, const Glib::ustring& name)
{
	std::stringstream ss;
	ss << device_path << " name " << partition_number << " " << name;

	Glib::ustring output = parted_cmd(ss.str());

	if (output.find("Error:") != Glib::ustring::npos)
	{
		m_last_error = output;
		return false;
	}
	return true;
}

bool PartedCommand::move(const Glib::ustring& device_path, int partition_number, Sector start, Sector end)
{
	std::stringstream ss;
	ss << device_path << " move " << partition_number << " " << start << "s " << end << "s";

	Glib::ustring output = parted_cmd(ss.str());

	if (output.find("Error:") != Glib::ustring::npos)
	{
		m_last_error = output;
		return false;
	}
	return true;
}

Glib::ustring PartedCommand::get_last_error() const
{
	return m_last_error;
}

Glib::ustring PartedCommand::sector_to_parted_size(Sector sectors, int sector_size)
{
	Sector bytes = sectors * sector_size;
	std::stringstream ss;

	if (bytes >= TEBIBYTE)
	{
		ss << ((double)bytes / TEBIBYTE) << "TiB";
	}
	else if (bytes >= GIBIBYTE)
	{
		ss << ((double)bytes / GIBIBYTE) << "GiB";
	}
	else if (bytes >= MEBIBYTE)
	{
		ss << ((double)bytes / MEBIBYTE) << "MiB";
	}
	else if (bytes >= KIBIBYTE)
	{
		ss << ((double)bytes / KIBIBYTE) << "KiB";
	}
	else
	{
		ss << bytes << "B";
	}

	return ss.str();
}

Glib::ustring PartedCommand::parted_cmd(const Glib::ustring& args)
{
	return m_adb.shell_command("parted -s " + args);
}

} // namespace GParted