#include "adb/PartedParser.h"
#include "GParted_Core.h"
#include "Utils.h"

#include <sstream>
#include <iostream>
#include <cmath>

namespace GParted
{

PartedParser::PartedParser()
{
}

std::vector<Glib::ustring> PartedParser::parse_device_paths(const Glib::ustring& output)
{
	std::vector<Glib::ustring> paths;
	std::stringstream ss(output.raw());
	std::string line;

	while (std::getline(ss, line))
	{
		Glib::ustring ustr(line);

		if (ustr.find("Model:") == 0)
		{
			continue;
		}

		if (ustr.find("Disk ") == 0)
		{
			if (ustr.find("Disk Flags:") == 0)
				continue;

			size_t colon_pos = ustr.find(':');
			if (colon_pos != Glib::ustring::npos)
			{
				Glib::ustring path = trim(ustr.substr(5, colon_pos - 5));
				if (!path.empty())
					paths.push_back(path);
			}
			continue;
		}

		if (ustr.find("/dev/") == 0)
		{
			size_t colon_pos = ustr.find(':');
			if (colon_pos != Glib::ustring::npos)
			{
				paths.push_back(trim(ustr.substr(0, colon_pos)));
			}
		}
	}

	return paths;
}

bool PartedParser::parse_device_info(const Glib::ustring& output, Device& device)
{
	device.Reset();
	device.sector_size = 512;

	std::stringstream ss(output.raw());
	std::string line;
	bool found_disk = false;

	while (std::getline(ss, line))
	{
		Glib::ustring ustr(line);

		if (ustr.find("Model:") == 0)
		{
			Glib::ustring model = trim(ustr.substr(6));
			model = model.substr(0, model.size() - 1);
			device.model = model;
			continue;
		}

		if (ustr.find("Disk ") == 0)
		{
			if (ustr.find("Disk Flags:") == 0)
				continue;

			size_t colon_pos = ustr.find(':');
			if (colon_pos != Glib::ustring::npos)
			{
				Glib::ustring path = trim(ustr.substr(5, colon_pos - 5));
				device.set_path(path);
				found_disk = true;

				Glib::ustring after_colon = ustr.substr(colon_pos + 1);
				size_t size_pos = after_colon.find(":");
				if (size_pos != Glib::ustring::npos)
				{
					Glib::ustring size_str = trim(after_colon.substr(0, size_pos));
				}
			}
			continue;
		}

		if (ustr.find("Sector size (logical/physical):") == 0)
		{
			Glib::ustring rest = ustr.substr(31);
			size_t slash_pos = rest.find('/');
			if (slash_pos != Glib::ustring::npos)
			{
				Glib::ustring logical_str = trim(rest.substr(0, slash_pos));
				size_t b_pos = logical_str.find('B');
				if (b_pos != Glib::ustring::npos)
				{
					device.sector_size = std::atoi(logical_str.substr(0, b_pos).c_str());
				}
			}
			continue;
		}

		if (ustr.find("Partition Table:") == 0)
		{
			device.disktype = trim(ustr.substr(16));
			continue;
		}

		if (ustr.find("Disk Flags:") == 0)
		{
			continue;
		}
	}

	if (!found_disk)
		return false;

	device.heads = 255;
	device.sectors = 63;
	device.cylinders = device.length / (device.heads * device.sectors);
	device.cylsize = device.heads * device.sectors;
	device.max_prims = 128;

	if (device.cylsize < (MEBIBYTE / device.sector_size))
		device.cylsize = MEBIBYTE / device.sector_size;

	return true;
}

bool PartedParser::parse_partitions(const Glib::ustring& output, Device& device)
{
	device.partitions.clear();

	std::stringstream ss(output.raw());
	std::string line;
	bool in_partition_table = false;
	int ext_index = -1;
	size_t num_col = 0, start_col = 0, end_col = 0, size_col = 0, fs_col = 0, name_col = 0, flags_col = 0;

	while (std::getline(ss, line))
	{
		Glib::ustring ustr(line);

		if (ustr.find("Disk ") == 0)
		{
			if (ustr.find("Disk Flags:") == 0)
				continue;

			size_t colon_pos = ustr.find(':');
			if (colon_pos != Glib::ustring::npos)
			{
				Glib::ustring path = trim(ustr.substr(5, colon_pos - 5));
				device.set_path(path);
			}
		}

		if (ustr.find("Number") == 0 && ustr.find("Start") != Glib::ustring::npos)
		{
			in_partition_table = true;
			num_col   = ustr.find("Number");
			start_col = ustr.find("Start");
			end_col   = ustr.find("End");
			size_col  = ustr.find("Size", end_col + 3);
			fs_col    = ustr.find("File system");
			name_col  = ustr.find("Name", fs_col + 11);
			flags_col = ustr.find("Flags", name_col + 4);
			continue;
		}

		if (in_partition_table && !ustr.empty())
		{
			Partition* partition = new Partition();

			Glib::ustring number_str = trim(ustr.substr(num_col, start_col - num_col));
			int partition_number = std::atoi(number_str.c_str());

			if (partition_number == 0)
			{
				delete partition;
				continue;
			}

			partition->partition_number = partition_number;

			Glib::ustring start_str = trim(ustr.substr(start_col, end_col - start_col));
			Glib::ustring end_str   = trim(ustr.substr(end_col, size_col - end_col));

			Sector start = 0;
			Sector end = 0;

			if (start_str.find('s') != Glib::ustring::npos)
			{
				start_str = start_str.substr(0, start_str.size() - 1);
				start = std::atoll(start_str.c_str());
			}
			else
			{
				start = parse_parted_size(start_str, device.sector_size);
			}
			partition->sector_start = start;

			if (end_str.find('s') != Glib::ustring::npos)
			{
				end_str = end_str.substr(0, end_str.size() - 1);
				end = std::atoll(end_str.c_str());
			}
			else
			{
				end = parse_parted_size(end_str, device.sector_size);
			}
			partition->sector_end = end;

			Glib::ustring padded = ustr;
			if (padded.size() < flags_col)
				padded.append(flags_col - padded.size(), ' ');
			Glib::ustring fs_str = trim(padded.substr(fs_col, name_col - fs_col));
			Glib::ustring name_str = trim(padded.substr(name_col, flags_col - name_col));
			Glib::ustring flags_str = trim(padded.substr(flags_col));

			FSType fstype = FS_UNKNOWN;
			if (!fs_str.empty())
			{
				fstype = parse_fs_type(fs_str);
			}

			partition->name = name_str;
			partition->fstype = fstype;

			std::stringstream flagss(flags_str.raw());
			std::string flag;
			while (std::getline(flagss, flag, ','))
			{
				Glib::ustring flag_trimmed = trim(Glib::ustring(flag));
				if (!flag_trimmed.empty())
					partition->flags.push_back(flag_trimmed);
			}

			PartitionType part_type = TYPE_PRIMARY;
			for (const auto& flag : partition->flags)
			{
				if (flag == "extended" || flag == "lba")
				{
					part_type = TYPE_EXTENDED;
					partition->fstype = FS_EXTENDED;
					ext_index = device.partitions.size();
					break;
				}
				if (flag == "logical")
				{
					part_type = TYPE_LOGICAL;
					break;
				}
			}
			partition->type = part_type;

			partition->device_path = device.get_path();
			partition->sector_size = device.sector_size;
			partition->inside_extended = (part_type == TYPE_LOGICAL);
			partition->busy = false;
			partition->status = STAT_REAL;

			partition->set_path(device.get_path() + Utils::num_to_str(partition_number));

			if (part_type == TYPE_LOGICAL && ext_index >= 0)
			{
				device.partitions[ext_index].logicals.push_back_adopt(partition);
			}
			else
			{
				device.partitions.push_back_adopt(partition);
			}
		}
	}

	GParted_Core::insert_unallocated(device.get_path(), device.partitions, 0, device.length - 1, device.sector_size, false);

	return true;
}

std::vector<Glib::ustring> PartedParser::parse_disk_label_types(const Glib::ustring& output)
{
	std::vector<Glib::ustring> types;
	std::stringstream ss(output.raw());
	std::string line;
	bool in_table = false;

	while (std::getline(ss, line))
	{
		Glib::ustring ustr(line);

		if (ustr.find("Supported partition tables") != Glib::ustring::npos)
		{
			in_table = true;
			continue;
		}

		if (in_table && !ustr.empty())
		{
			Glib::ustring trimmed = trim(ustr);
			if (!trimmed.empty() && trimmed.find(" ") == Glib::ustring::npos)
			{
				types.push_back(trimmed);
			}
		}
	}

	return types;
}

std::map<Glib::ustring, bool> PartedParser::parse_available_flags(const Glib::ustring& output, int partition_number)
{
	std::map<Glib::ustring, bool> flags;
	std::stringstream ss(output.raw());
	std::string line;

	while (std::getline(ss, line))
	{
		Glib::ustring ustr(line);

		std::stringstream line_ss(ustr.raw());
		std::string token;
		std::vector<std::string> tokens;

		while (std::getline(line_ss, token, ' '))
		{
			if (!token.empty())
				tokens.push_back(token);
		}

		if (tokens.size() >= 4)
		{
			int num = std::atoi(tokens[0].c_str());
			if (num == partition_number)
			{
				Glib::ustring flag_name(tokens[2]);
				bool is_set = (tokens[3] == "on" || tokens[3] == "active");
				flags[flag_name] = is_set;
			}
		}
	}

	return flags;
}

int PartedParser::parse_sector_size(const Glib::ustring& output)
{
	std::stringstream ss(output.raw());
	std::string line;

	while (std::getline(ss, line))
	{
		Glib::ustring ustr(line);

		if (ustr.find("Sector size (logical/physical):") == 0)
		{
			Glib::ustring rest = ustr.substr(31);
			size_t slash_pos = rest.find('/');
			if (slash_pos != Glib::ustring::npos)
			{
				Glib::ustring logical_str = trim(rest.substr(0, slash_pos));
				size_t b_pos = logical_str.find('B');
				if (b_pos != Glib::ustring::npos)
				{
					return std::atoi(logical_str.substr(0, b_pos).c_str());
				}
			}
		}
	}

	return 512;
}

Sector PartedParser::parse_parted_size(const Glib::ustring& size_str, int sector_size)
{
	Glib::ustring trimmed = trim(size_str);

	if (trimmed.empty())
		return 0;

	size_t unit_pos = trimmed.size();
	for (size_t i = 0; i < trimmed.size(); i++)
	{
		if (!isdigit(trimmed[i]) && trimmed[i] != '.')
		{
			unit_pos = i;
			break;
		}
	}

	std::string num_str = trimmed.substr(0, unit_pos);
	double value = std::atof(num_str.c_str());

	if (unit_pos >= trimmed.size())
	{
		return static_cast<Sector>(value / sector_size);
	}

	Glib::ustring unit = trimmed.substr(unit_pos);

	if (unit == "B")
		return static_cast<Sector>(value / sector_size);
	else if (unit == "KiB" || unit == "kB")
		return static_cast<Sector>((value * 1024) / sector_size);
	else if (unit == "MiB" || unit == "MB")
		return static_cast<Sector>((value * 1024 * 1024) / sector_size);
	else if (unit == "GiB" || unit == "GB")
		return static_cast<Sector>((value * 1024.0 * 1024.0 * 1024.0) / sector_size);
	else if (unit == "TiB" || unit == "TB")
		return static_cast<Sector>((value * 1024.0 * 1024.0 * 1024.0 * 1024.0) / sector_size);
	else if (unit == "s")
		return static_cast<Sector>(value);

	return static_cast<Sector>(value / sector_size);
}

FSType PartedParser::parse_fs_type(const Glib::ustring& fs_name)
{
	Glib::ustring name = trim(fs_name);

	if (name == "ext2")
		return FS_EXT2;
	if (name == "ext3")
		return FS_EXT3;
	if (name == "ext4")
		return FS_EXT4;
	if (name == "linux-swap" || name == "linux-swap(v1)")
		return FS_LINUX_SWAP;
	if (name == "fat16")
		return FS_FAT16;
	if (name == "fat32")
		return FS_FAT32;
	if (name == "vfat")
		return FS_FAT32;
	if (name == "ntfs")
		return FS_NTFS;
	if (name == "btrfs")
		return FS_BTRFS;
	if (name == "xfs")
		return FS_XFS;
	if (name == "jfs")
		return FS_JFS;
	if (name == "reiserfs")
		return FS_REISERFS;
	if (name == "reiser4")
		return FS_REISER4;
	if (name == "hfs")
		return FS_HFS;
	if (name == "hfs+")
		return FS_HFSPLUS;
	if (name == "ufs")
		return FS_UFS;
	if (name == "exfat")
		return FS_EXFAT;
	if (name == "f2fs")
		return FS_F2FS;
	if (name == "nilfs2")
		return FS_NILFS2;
	if (name == "minix")
		return FS_MINIX;
	if (name == "udf")
		return FS_UDF;
	if (name == "lvm2" || name == "lvm2 pv")
		return FS_LVM2_PV;
	if (name == "luks")
		return FS_LUKS;
	if (name == "zfs")
		return FS_ZFS;
	if (name == "bcache")
		return FS_BCACHE;
	if (name == "bitlocker")
		return FS_BITLOCKER;
	if (name == "iso9660")
		return FS_ISO9660;
	if (name == "apfs")
		return FS_APFS;
	if (name == "erofs")
		return FS_EROFS;

	return FS_UNKNOWN;
}

void PartedParser::split_line(const Glib::ustring& line, char delimiter, std::vector<Glib::ustring>& parts)
{
	std::stringstream ss(line.raw());
	std::string part;

	while (std::getline(ss, part, delimiter))
	{
		parts.push_back(Glib::ustring(part));
	}
}

Glib::ustring PartedParser::trim(const Glib::ustring& str)
{
	size_t start = 0;
	size_t end = str.size();

	while (start < end && (str[start] == ' ' || str[start] == '\t' || str[start] == '\r' || str[start] == '\n'))
		start++;

	while (end > start && (str[end - 1] == ' ' || str[end - 1] == '\t' || str[end - 1] == '\r' || str[end - 1] == '\n'))
		end--;

	if (start >= end)
		return "";

	return str.substr(start, end - start);
}

} // namespace GParted