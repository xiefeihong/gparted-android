/* Copyright (C) 2024 GParted-Android Project
 * Copyright (C) 2004 Bart Hakvoort
 * Copyright (C) 2008-2024 Curtis Gedak
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This is the ADB-based backend for GParted. It replaces libparted calls
 * with ADB shell commands that invoke the parted CLI tool on an Android
 * device (typically in recovery mode).
 */

#include "GParted_Core.h"
#include "adb/AdbClient.h"
#include "adb/PartedCommand.h"
#include "adb/PartedParser.h"
#include "BlockSpecial.h"
#include "CopyBlocks.h"
#include "Device.h"
#include "DMRaid.h"
#include "FileSystem.h"
#include "FS_Info.h"
#include "LVM2_PV_Info.h"
#include "LUKS_Info.h"
#include "Mount_Info.h"
#include "Operation.h"
#include "OperationCopy.h"
#include "Partition.h"
#include "PartitionLUKS.h"
#include "PartitionVector.h"
#include "Proc_Partitions_Info.h"
#include "SupportedFileSystems.h"
#include "SWRaid_Info.h"
#include "Utils.h"
#include "../config.h"
#include "btrfs.h"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <map>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <endian.h>
#include <glibmm/miscutils.h>
#include <glibmm/fileutils.h>
#include <glibmm/shell.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/main.h>

#ifdef USE_ADB_BACKEND

namespace GParted
{

Glib::Thread* GParted_Core::mainthread = nullptr;


const std::time_t SETTLE_DEVICE_PROBE_MAX_WAIT_SECONDS = 1;
const std::time_t SETTLE_DEVICE_APPLY_MAX_WAIT_SECONDS = 10;

static const Glib::ustring GPARTED_BUG(_("GParted Bug"));

static AdbClient* g_adb = nullptr;
static PartedCommand* g_parted_cmd = nullptr;
static PartedParser* g_parted_parser = nullptr;

struct AdbScanCache
{
	bool valid = false;
	Glib::ustring combined_parted_output;
	std::map<Glib::ustring, std::vector<Glib::ustring>> mount_map;
	std::map<Glib::ustring, Glib::ustring> fstype_map;
};

static AdbScanCache g_scan_cache;

static Glib::ustring extract_device_output(const Glib::ustring& combined, const Glib::ustring& device_path)
{
	Glib::ustring marker = "Disk " + device_path + ":";
	Glib::ustring::size_type start = combined.find(marker);
	if (start == Glib::ustring::npos)
		return "";

	Glib::ustring::size_type end = combined.find("\nDisk /dev/", start + marker.size());
	if (end == Glib::ustring::npos)
		end = combined.size();

	return combined.substr(start, end - start);
}

static AdbClient& adb()
{
	if (!g_adb)
	{
		g_adb = new AdbClient();
	}
	return *g_adb;
}

static PartedCommand& parted_cmd()
{
	if (!g_parted_cmd)
	{
		g_parted_cmd = new PartedCommand(adb());
	}
	return *g_parted_cmd;
}

static PartedParser& parted_parser()
{
	if (!g_parted_parser)
	{
		g_parted_parser = new PartedParser();
	}
	return *g_parted_parser;
}

GParted_Core::GParted_Core()
{
	thread_status_message = "";

	find_supported_core();

	supported_filesystems = std::make_unique<SupportedFileSystems>();
	supported_filesystems->find_supported_filesystems();
}

GParted_Core::~GParted_Core()
{
	delete g_parted_parser;
	g_parted_parser = nullptr;
	delete g_parted_cmd;
	g_parted_cmd = nullptr;
	delete g_adb;
	g_adb = nullptr;
}


Glib::ustring GParted_Core::get_version_and_config_string()
{
	Glib::ustring str = Glib::ustring("GParted-Android ") + VERSION + "\n";
	str += "configuration --enable-adb-backend\n";
	str += Glib::ustring("libparted: none (ADB parted backend)\n");
	return str;
}


// --- Device Scanning ---

void GParted_Core::set_user_devices(const std::vector<Glib::ustring>& user_devices)
{
	device_paths = user_devices;
	probe_devices = device_paths.empty();
}

void GParted_Core::set_devices(std::vector<Device>& devices)
{
	devices.clear();
	device_paths.clear();
	g_scan_cache = AdbScanCache();

	if (probe_devices)
	{
		log_set("[set_devices] probing devices via parted -l...");
		Glib::ustring output = parted_cmd().print_all("s");
		log_set(("[set_devices] parted output size=" + Utils::num_to_str(output.size())).c_str());
		g_scan_cache.combined_parted_output = output;

		std::vector<Glib::ustring> paths = parted_parser().parse_device_paths(output);
		if (enable_log_set)
		{
			std::cerr << "[set_devices] parsed " << paths.size() << " device paths:" << std::endl;
			for (const auto& p : paths)
				std::cerr << "  -> " << p << std::endl;
		}
		device_paths = paths;

		DMRaid dmraid;
		std::vector<Glib::ustring> dmraid_devices;
		if (dmraid.is_dmraid_supported())
		{
			dmraid_devices = DMRaid::get_devices();
			for (unsigned int k = 0; k < dmraid_devices.size(); k++)
			{
				set_thread_status_message(
					Glib::ustring::compose(_("Scanning %1"), dmraid_devices[k]));
				dmraid.create_dev_map_entries(dmraid_devices[k]);
			}
		}
	}

	std::sort(device_paths.begin(), device_paths.end());

	Glib::ustring proc_mounts_out;
	Glib::ustring proc_mounts_err;
	adb().shell_command_exit_status("cat /proc/mounts", proc_mounts_out, proc_mounts_err);
	log_set(("[set_devices] /proc/mounts size=" + Utils::num_to_str(proc_mounts_out.size())).c_str());

	if (!proc_mounts_out.empty())
	{
		std::stringstream pmss(proc_mounts_out.raw());
		std::string pml;
		while (std::getline(pmss, pml))
		{
			Glib::ustring line(pml);
			std::stringstream tokens(line.raw());
			std::string dev, mp, fs;
			tokens >> dev >> mp >> fs;
			if (!dev.empty() && dev[0] == '/' && !mp.empty())
			{
				Glib::ustring d(dev);
				g_scan_cache.mount_map[d].push_back(Glib::ustring(mp));
				if (!fs.empty())
					g_scan_cache.fstype_map[d] = Glib::ustring(fs);
			}
		}
	}

	Glib::ustring mount_cmd_out;
	Glib::ustring mount_cmd_err;
	adb().shell_command_exit_status("mount", mount_cmd_out, mount_cmd_err);
	log_set(("[set_devices] mount size=" + Utils::num_to_str(mount_cmd_out.size())).c_str());

	if (!mount_cmd_out.empty())
	{
		std::stringstream mcss(mount_cmd_out.raw());
		std::string mcl;
		while (std::getline(mcss, mcl))
		{
			Glib::ustring line(mcl);
			size_t on_pos = line.find(" on ");
			size_t type_pos = line.find(" type ");
			if (on_pos != Glib::ustring::npos)
			{
				Glib::ustring dev = Utils::trim(line.substr(0, on_pos));
				Glib::ustring mp;
				Glib::ustring fs;
				if (type_pos != Glib::ustring::npos)
				{
					mp = Utils::trim(line.substr(on_pos + 4, type_pos - on_pos - 4));
					Glib::ustring after_type = line.substr(type_pos + 6);
					size_t paren = after_type.find('(');
					if (paren != Glib::ustring::npos)
						fs = Utils::trim(after_type.substr(0, paren));
					else
						fs = Utils::trim(after_type);
				}
				else
				{
					mp = Utils::trim(line.substr(on_pos + 4));
				}

				if (!dev.empty() && dev[0] == '/' && !mp.empty())
				{
					bool found = false;
					for (const auto& existing : g_scan_cache.mount_map[dev])
					{
						if (existing == mp) { found = true; break; }
					}
					if (!found)
					{
						g_scan_cache.mount_map[dev].push_back(mp);
						if (!fs.empty() && g_scan_cache.fstype_map.find(dev) == g_scan_cache.fstype_map.end())
							g_scan_cache.fstype_map[dev] = fs;
					}
				}
			}
		}
	}

	g_scan_cache.valid = true;

	FS_Info::clear_cache();
	const std::vector<DeviceAndPartitionNames> device_and_partition_paths =
		Proc_Partitions_Info::get_device_and_partition_names_for(device_paths);
	FS_Info::load_cache_for_device_and_partition_names(device_and_partition_paths);
	Mount_Info::load_cache();
	LVM2_PV_Info::clear_cache();
	btrfs::clear_cache();
	SWRaid_Info::load_cache();
	LUKS_Info::clear_cache();

	for (unsigned int t = 0; t < device_paths.size(); t++)
	{
		set_thread_status_message(
			Glib::ustring::compose(_("Searching %1 partitions"), device_paths[t]));
		Device temp_device;
		set_device_from_disk(temp_device, device_paths[t]);
		devices.push_back(temp_device);
	}

	set_thread_status_message("");
}

void GParted_Core::set_devices_thread(std::vector<Device>* pdevices)
{
	set_devices(*pdevices);
}

void GParted_Core::set_device_from_disk(Device& device, const Glib::ustring& device_path)
{
	device.Reset();

	log_set("[set_device_from_disk] querying...");
	Glib::ustring output;
	if (g_scan_cache.valid && !g_scan_cache.combined_parted_output.empty())
	{
		output = extract_device_output(g_scan_cache.combined_parted_output, device_path);
		log_set(("[set_device_from_disk] using cached output size=" + Utils::num_to_str(output.size())).c_str());
	}
	if (output.empty())
	{
		output = parted_cmd().print_device(device_path, "s");
		log_set(("[set_device_from_disk] direct query output size=" + Utils::num_to_str(output.size())).c_str());
	}

	if (output.empty() || output.find("unrecognised") != Glib::ustring::npos)
	{
		device.set_path(device_path);
		device.model = "Unknown Device";
		device.length = 0;
		device.sector_size = 512;
		device.heads = 255;
		device.sectors = 63;
		device.cylinders = 0;
		device.cylsize = MEBIBYTE / device.sector_size;
		device.disktype = _("unrecognized");
		device.max_prims = 1;

		Partition* partition_temp = new Partition();
		partition_temp->set_unpartitioned(device.get_path(), "",
		                                  FS_UNALLOCATED, device.length,
		                                  device.sector_size, false);
		device.partitions.push_back_adopt(partition_temp);
		return;
	}

	parted_parser().parse_device_info(output, device);

	std::stringstream ss(output.raw());
	std::string line;
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
				Glib::ustring after = ustr.substr(colon_pos + 1);
				after = Utils::trim(after);

				size_t second_colon = after.find(':');
				if (second_colon != Glib::ustring::npos)
				{
					after = after.substr(0, second_colon);
				}

				size_t space_pos = after.find(' ');
				Glib::ustring size_str = after;
				Glib::ustring unit_str;
				if (space_pos != Glib::ustring::npos)
				{
					size_str = after.substr(0, space_pos);
					unit_str = after.substr(space_pos + 1);
				}
				else
				{
					size_t i = 0;
					while (i < after.size() && (std::isdigit(after[i]) || after[i] == '.'))
						i++;
					size_str = after.substr(0, i);
					unit_str = after.substr(i);
				}

				if (!unit_str.empty() && unit_str[unit_str.size() - 1] == 's')
				{
					unit_str = unit_str.substr(0, unit_str.size() - 1);
				}

				double size_val = std::atof(size_str.c_str());

				if (unit_str.empty() || unit_str == "s")
				{
					device.length = static_cast<Sector>(size_val);
				}
				else
				{
					long long bytes = static_cast<long long>(size_val);
					if (unit_str.find("TiB") != Glib::ustring::npos || unit_str.find("TB") != Glib::ustring::npos)
						bytes = static_cast<long long>(size_val * 1024.0 * 1024.0 * 1024.0 * 1024.0);
					else if (unit_str.find("GiB") != Glib::ustring::npos || unit_str.find("GB") != Glib::ustring::npos)
						bytes = static_cast<long long>(size_val * 1024.0 * 1024.0 * 1024.0);
					else if (unit_str.find("MiB") != Glib::ustring::npos || unit_str.find("MB") != Glib::ustring::npos)
						bytes = static_cast<long long>(size_val * 1024.0 * 1024.0);
					else if (unit_str.find("KiB") != Glib::ustring::npos || unit_str.find("KB") != Glib::ustring::npos)
						bytes = static_cast<long long>(size_val * 1024.0);
					else if (unit_str.find("B") != Glib::ustring::npos)
						bytes = static_cast<long long>(size_val);
					device.length = bytes / device.sector_size;
				}
			}
		}
	}

	device.heads = 255;
	device.sectors = 63;
	device.cylinders = device.length / (device.heads * device.sectors);
	device.cylsize = device.heads * device.sectors;

	int sec_size = device.sector_size > 0 ? device.sector_size : 512;
	if (device.cylsize < (MEBIBYTE / sec_size))
		device.cylsize = MEBIBYTE / sec_size;

	if (device.disktype == "none" || device.disktype.empty() ||
	    device.disktype == _("unrecognized"))
	{
		device.max_prims = 1;

		std::vector<Glib::ustring> messages;
		FSType fstype = FS_UNKNOWN;
		Glib::ustring cmd = "blkid -s TYPE -o value ";
		cmd += Glib::shell_quote(device_path);
		Glib::ustring blkid_output = adb().shell_command(cmd);
		blkid_output = Utils::trim(blkid_output);
		if (!blkid_output.empty())
		{
			fstype = parted_parser().parse_fs_type(blkid_output);
		}
		else
		{
			fstype = FS_UNKNOWN;
		}

		Partition* partition_temp = new Partition();
		partition_temp->set_unpartitioned(device.get_path(), device_path,
		                                  fstype, device.length,
		                                  device.sector_size, false);
		device.partitions.push_back_adopt(partition_temp);
	}
	else
	{
		if (device.disktype == "gpt")
			device.max_prims = 128;
		else if (device.disktype == "msdos")
			device.max_prims = 4;
		else
			device.max_prims = 1;

		parted_parser().parse_partitions(output, device);

		std::vector<Glib::ustring> all_partition_paths;
		for (unsigned int i = 0; i < device.partitions.size(); i++)
		{
			if (device.partitions[i].type != TYPE_UNALLOCATED &&
			    device.partitions[i].type != TYPE_EXTENDED)
			{
				Glib::ustring p = device.get_path();
				p += Utils::num_to_str(device.partitions[i].partition_number);
				all_partition_paths.push_back(p);
			}
		}

		const std::map<Glib::ustring, std::vector<Glib::ustring>>& mount_map = g_scan_cache.mount_map;
		const std::map<Glib::ustring, Glib::ustring>& fstype_map = g_scan_cache.fstype_map;

		if (!all_partition_paths.empty())
		{
			Glib::ustring blkid_cmd = "blkid";
			for (const auto& p : all_partition_paths)
				blkid_cmd += " " + Glib::shell_quote(p);

			Glib::ustring blkid_out;
			Glib::ustring blkid_err;
			int blkid_rc = adb().shell_command_exit_status(blkid_cmd, blkid_out, blkid_err);
			log_set(("[set_device_from_disk] blkid rc=" + Utils::num_to_str(blkid_rc)).c_str());
			log_out(("[set_device_from_disk] blkid output=[" + blkid_out + "]").c_str());

			if (blkid_rc == 0 && !blkid_out.empty())
			{
				std::stringstream bss(blkid_out.raw());
				std::string bline;
				while (std::getline(bss, bline))
				{
					Glib::ustring bu(bline);
					size_t colon = bu.find(':');
					if (colon == Glib::ustring::npos) continue;

					Glib::ustring path = Utils::trim(bu.substr(0, colon));
					Glib::ustring rest = bu.substr(colon + 1);

					Glib::ustring type;
					Glib::ustring trest = rest;
					size_t tpos = trest.find(" TYPE=\"");
					if (tpos != Glib::ustring::npos)
						trest = trest.substr(tpos + 1);
					else
					{
						tpos = trest.find("TYPE=\"");
					}
					if (tpos != Glib::ustring::npos)
					{
						size_t val_start = trest.find("\"") + 1;
						size_t val_end   = trest.find("\"", val_start);
						if (val_end != Glib::ustring::npos)
							type = trest.substr(val_start, val_end - val_start);
					}

					Glib::ustring label;
					Glib::ustring lrest = rest;
					size_t lpos = lrest.find("LABEL=\"");
					if (lpos != Glib::ustring::npos)
					{
						lrest = lrest.substr(lpos);
						size_t val_start = lrest.find("\"") + 1;
						size_t val_end   = lrest.find("\"", val_start);
						if (val_end != Glib::ustring::npos)
							label = lrest.substr(val_start, val_end - val_start);
					}

					Glib::ustring uuid;
					Glib::ustring urest = rest;
					size_t upos = urest.find("UUID=\"");
					if (upos != Glib::ustring::npos)
					{
						urest = urest.substr(upos);
						size_t val_start = urest.find("\"") + 1;
						size_t val_end   = urest.find("\"", val_start);
						if (val_end != Glib::ustring::npos)
							uuid = urest.substr(val_start, val_end - val_start);
					}

					for (unsigned int i = 0; i < device.partitions.size(); i++)
					{
						Glib::ustring pp = device.get_path();
						pp += Utils::num_to_str(device.partitions[i].partition_number);
						if (pp == path)
						{
							if (!type.empty())
							{
								FSType parted_fs = device.partitions[i].fstype;
								if (parted_fs == FS_UNKNOWN || parted_fs == FS_UNALLOCATED)
								{
									FSType detected = parted_parser().parse_fs_type(type);
									if (detected != FS_UNKNOWN)
									{
										log_set(("[set_device_from_disk] blkid: " + path + " type=" + type).c_str());
										device.partitions[i].fstype = detected;
									}
								}
							}
							if (!label.empty())
							{
								log_set(("[set_device_from_disk] blkid: " + path + " label=" + label).c_str());
								device.partitions[i].set_filesystem_label(label);
							}
							if (!uuid.empty())
							{
								log_set(("[set_device_from_disk] blkid: " + path + " uuid=" + uuid).c_str());
								device.partitions[i].uuid = uuid;
							}
							break;
						}
					}
				}
			}
		}

		for (unsigned int i = 0; i < device.partitions.size(); i++)
		{
			if (device.partitions[i].type == TYPE_UNALLOCATED ||
			    device.partitions[i].type == TYPE_EXTENDED)
				continue;

			Glib::ustring pp = device.get_path();
			pp += Utils::num_to_str(device.partitions[i].partition_number);

			auto mit = mount_map.find(pp);
			if (mit != mount_map.end() && !mit->second.empty())
			{
				device.partitions[i].busy = true;
				for (const auto& mp : mit->second)
					device.partitions[i].add_mountpoint(mp);
				log_set(("[set_device_from_disk] " + pp + " has " + Utils::num_to_str(mit->second.size()) + " mountpoints").c_str());
			}

			auto fit = fstype_map.find(pp);
			if (fit != fstype_map.end() && !fit->second.empty())
			{
				FSType parted_fs = device.partitions[i].fstype;
				if (parted_fs == FS_UNKNOWN || parted_fs == FS_UNALLOCATED)
				{
					FSType detected = parted_parser().parse_fs_type(fit->second);
					if (detected != FS_UNKNOWN)
					{
						device.partitions[i].fstype = detected;
						log_set(("[set_device_from_disk] fstype from mounts: " + pp + " -> " + fit->second).c_str());
					}
				}
			}
		}

		for (unsigned int i = 0; i < device.partitions.size(); i++)
			set_used_sectors(device.partitions[i], nullptr);
	}
}


// --- Disk Label ---

bool GParted_Core::set_disklabel(const Device& device, const Glib::ustring& disklabel)
{
	const Glib::ustring& device_path = device.get_path();

	OperationDetail dummy_od;
	Partition temp_partition;
	temp_partition.set_unpartitioned(device_path, "", FS_UNALLOCATED,
	                                  device.length, device.sector_size, false);
	erase_filesystem_signatures(temp_partition, dummy_od);

	return new_disklabel(device_path, disklabel);
}

bool GParted_Core::new_disklabel(const Glib::ustring& device_path,
                                 const Glib::ustring& disklabel,
                                 bool recreate_dmraid_devs)
{
	return parted_cmd().mklabel(device_path, disklabel);
}

std::vector<Glib::ustring> GParted_Core::get_disklabeltypes()
{
	Glib::ustring output = adb().shell_command("parted -h");
	return parted_parser().parse_disk_label_types(output);
}


// --- Flags ---

bool GParted_Core::toggle_flag(const Partition& partition,
                               const Glib::ustring& flag, bool state)
{
	return parted_cmd().set_flag(partition.device_path,
	                             partition.partition_number, flag, state);
}

std::map<Glib::ustring, bool> GParted_Core::get_available_flags(const Partition& partition)
{
	Glib::ustring output = parted_cmd().print_device(partition.device_path, "s");
	return parted_parser().parse_available_flags(output, partition.partition_number);
}


// --- Partition Operations ---

bool GParted_Core::valid_partition(const Device& device, Partition& partition,
                                   Glib::ustring& error)
{
	if (partition.sector_start < 0)
	{
		error = GPARTED_BUG + ": " +
		        Glib::ustring::compose(
		            _("A partition cannot start (%1) before the start of the device"),
		            partition.sector_start);
		return false;
	}
	if (partition.sector_end >= device.length)
	{
		error = GPARTED_BUG + ": " +
		        Glib::ustring::compose(
		            _("A partition cannot end (%1) after the end of the device (%2)"),
		            partition.sector_start);
		return false;
	}
	if (partition.get_sector_length() <= 0)
	{
		error = GPARTED_BUG + ": " +
		        Glib::ustring::compose(
		            _("A partition cannot have a length of %1 sectors"),
		            partition.get_sector_length());
		return false;
	}
	if (partition.get_sector_length() < partition.sectors_used)
	{
		error = GPARTED_BUG + ": " +
		        Glib::ustring::compose(
		            _("A partition with used sectors (%1) greater than its length (%2) is not valid"),
		            partition.sectors_used,
		            partition.get_sector_length());
		return false;
	}
	return true;
}


Glib::ustring GParted_Core::check_logical_esp_warning(PartitionType ptntype, bool esp_flag)
{
	return "";
}

void GParted_Core::compose_partition_flags(Partition& partition, const Glib::ustring& disktype)
{
}


// --- File Systems ---

const std::vector<FS>& GParted_Core::get_filesystems() const
{
	return supported_filesystems->get_all_fs_support();
}

const FS& GParted_Core::get_fs(FSType fstype) const
{
	return supported_filesystems->get_fs_support(fstype);
}

void GParted_Core::find_supported_filesystems()
{
	supported_filesystems->find_supported_filesystems();
}

void GParted_Core::find_supported_core()
{
}

FileSystem* GParted_Core::get_filesystem_object(FSType fstype)
{
	return supported_filesystems->get_fs_object(fstype);
}

bool GParted_Core::supported_filesystem(FSType fstype)
{
	return supported_filesystems->get_fs_object(fstype) != nullptr;
}

FS_Limits GParted_Core::get_filesystem_limits(FSType fstype, const Partition& partition)
{
	FS_Limits fs_limits;
	FileSystem* p_filesystem = get_filesystem_object(fstype);
	if (p_filesystem != NULL)
		fs_limits = p_filesystem->get_filesystem_limits(partition);
	return fs_limits;
}

bool GParted_Core::filesystem_resize_disallowed(const Partition& partition)
{
	if (partition.fstype == FS_UNALLOCATED)
		return true;

	return false;
}

void GParted_Core::insert_unallocated(const Glib::ustring& device_path,
                                      PartitionVector& partitions,
                                      Sector start, Sector end,
                                      Byte_Value sector_size, bool inside_extended)
{
	if (partitions.empty())
	{
		Partition* partition_temp = new Partition();
		partition_temp->Set_Unallocated(device_path, start, end, sector_size, inside_extended);
		partitions.push_back_adopt(partition_temp);
		return;
	}

	if ((partitions.front().sector_start - start) > (MEBIBYTE / sector_size))
	{
		Sector temp_end = partitions.front().sector_start - 1;
		Partition* partition_temp = new Partition();
		partition_temp->Set_Unallocated(device_path, start, temp_end, sector_size, inside_extended);
		partitions.insert_adopt(partitions.begin(), partition_temp);
	}

	for (unsigned int t = 0; t < partitions.size() - 1; t++)
	{
		if (((partitions[t + 1].sector_start - partitions[t].sector_end - 1) > (MEBIBYTE / sector_size))
		    || ((partitions[t + 1].type != TYPE_LOGICAL)
		        && ((partitions[t + 1].sector_start - partitions[t].sector_end - 1) == (MEBIBYTE / sector_size))))
		{
			Sector temp_start = partitions[t].sector_end + 1;
			Sector temp_end = partitions[t + 1].sector_start - 1;
			Partition* partition_temp = new Partition();
			partition_temp->Set_Unallocated(device_path, temp_start, temp_end,
			                                sector_size, inside_extended);
			partitions.insert_adopt(partitions.begin() + ++t, partition_temp);
		}
	}

	if ((end - partitions.back().sector_end) >= (MEBIBYTE / sector_size))
	{
		Sector temp_start = partitions.back().sector_end + 1;
		Partition* partition_temp = new Partition();
		partition_temp->Set_Unallocated(device_path, temp_start, end, sector_size, inside_extended);
		partitions.push_back_adopt(partition_temp);
	}
}


// --- Operation Application ---

bool GParted_Core::apply_operation_to_disk(Operation* operation)
{
	bool success = false;

	switch (operation->m_type)
	{
		case OPERATION_DELETE:
			success =    calibrate_partition(operation->get_partition_original(),
			                                 operation->m_operation_detail)
			          && delete_partition(operation->get_partition_original(),
			                              operation->m_operation_detail);
			break;

		case OPERATION_CREATE:
			success = create(operation->get_partition_new(), operation->m_operation_detail);
			break;

		case OPERATION_RESIZE_MOVE:
			success = calibrate_partition(operation->get_partition_original(),
			                              operation->m_operation_detail);
			if (!success) break;
			operation->get_partition_new().set_path(
				operation->get_partition_original().get_path());
			success = resize_move(operation->get_partition_original(),
			                      operation->get_partition_new(),
			                      operation->m_operation_detail);
			break;

		case OPERATION_FORMAT:
			success = calibrate_partition(operation->get_partition_new(),
			                              operation->m_operation_detail);
			if (!success) break;
			operation->get_partition_original().set_path(
				operation->get_partition_new().get_path());
			success =    remove_filesystem(operation->get_partition_original().get_filesystem_partition(),
			                               operation->m_operation_detail)
			          && format(operation->get_partition_new().get_filesystem_partition(),
			                    operation->m_operation_detail);
			break;

		case OPERATION_COPY:
		{
			OperationCopy* copy_op = static_cast<OperationCopy*>(operation);
			success =    calibrate_partition(copy_op->get_partition_copied(),
			                                 copy_op->m_operation_detail)
			          && remove_filesystem(copy_op->get_partition_original().get_filesystem_partition(),
			                               copy_op->m_operation_detail)
			          && copy(copy_op->get_partition_copied(),
			                  copy_op->get_partition_new(),
			                  copy_op->m_operation_detail);
			break;
		}

		case OPERATION_LABEL_FILESYSTEM:
			success =    calibrate_partition(operation->get_partition_new(),
			                                 operation->m_operation_detail)
			          && label_filesystem(operation->get_partition_new().get_filesystem_partition(),
			                              operation->m_operation_detail);
			break;

		case OPERATION_NAME_PARTITION:
			success =    calibrate_partition(operation->get_partition_new(),
			                                 operation->m_operation_detail)
			          && name_partition(operation->get_partition_new(),
			                            operation->m_operation_detail);
			break;

		case OPERATION_CHANGE_UUID:
			success =    calibrate_partition(operation->get_partition_new(),
			                                 operation->m_operation_detail)
			          && change_filesystem_uuid(operation->get_partition_new().get_filesystem_partition(),
			                                    operation->m_operation_detail);
			break;

		default:
			break;
	}

	return success;
}


// --- Thread Status ---

Glib::ustring GParted_Core::get_thread_status_message()
{
	return thread_status_message;
}

void GParted_Core::set_thread_status_message(Glib::ustring msg)
{
	thread_status_message = msg;
}


// --- Private: create partition ---

bool GParted_Core::create(Partition& new_partition, OperationDetail& operationdetail)
{
	operationdetail.add_child(OperationDetail(_("create empty partition")));

	bool success = create_partition(new_partition, operationdetail);

	if (new_partition.fstype != FS_UNALLOCATED && new_partition.fstype != FS_UNKNOWN)
	{
		success = success && create_filesystem(new_partition, operationdetail);
	}

	return success;
}

bool GParted_Core::create_partition(Partition& new_partition, OperationDetail& operationdetail, Sector min_size)
{
	Glib::ustring part_type;
	switch (new_partition.type)
	{
		case TYPE_PRIMARY:
			part_type = "primary";
			break;
		case TYPE_LOGICAL:
			part_type = "logical";
			break;
		case TYPE_EXTENDED:
			part_type = "extended";
			break;
		default:
			part_type = "primary";
			break;
	}

	Glib::ustring fs_name = "ext2";
	switch (new_partition.fstype)
	{
		case FS_EXT2:   fs_name = "ext2"; break;
		case FS_EXT3:   fs_name = "ext2"; break;
		case FS_EXT4:   fs_name = "ext2"; break;
		case FS_FAT16:  fs_name = "fat16"; break;
		case FS_FAT32:  fs_name = "fat32"; break;
		case FS_NTFS:   fs_name = "ntfs"; break;
		case FS_LINUX_SWAP: fs_name = "linux-swap"; break;
		case FS_BTRFS:  fs_name = "btrfs"; break;
		case FS_XFS:    fs_name = "xfs"; break;
		case FS_F2FS:   fs_name = "ext2"; break;
		case FS_EXFAT:  fs_name = "ntfs"; break;
		default:        fs_name = "ext2"; break;
	}

	bool success = parted_cmd().mkpart(new_partition.device_path, part_type,
	                                   fs_name, new_partition.sector_start,
	                                   new_partition.sector_end);

	if (success)
	{
		new_partition.partition_number = 0;

		if (new_partition.fstype != FS_EXTENDED && new_partition.fstype != FS_UNALLOCATED)
		{
			settle_device(SETTLE_DEVICE_APPLY_MAX_WAIT_SECONDS);
			Glib::ustring part_path = new_partition.device_path;

			size_t last_digit_pos = part_path.size();
			while (last_digit_pos > 0 && isdigit(part_path[last_digit_pos - 1]))
				last_digit_pos--;

			part_path = part_path + Utils::num_to_str(new_partition.partition_number);
			new_partition.set_path(part_path);
		}

		new_partition.status = STAT_REAL;
	}

	return success;
}


// --- Private: delete partition ---

bool GParted_Core::delete_partition(const Partition& partition, OperationDetail& operationdetail)
{
	operationdetail.add_child(OperationDetail(_("delete partition")));

	bool success = parted_cmd().rm(partition.device_path, partition.partition_number);

#ifndef USE_ADB_BACKEND
	DMRaid dmraid;
	if (success && dmraid.is_dmraid_device(partition.device_path))
	{
		PedDevice* lp_device = nullptr;
		PedDisk* lp_disk = nullptr;
		if (get_device_and_disk(partition.device_path, lp_device, lp_disk))
		{
		}
	}
#endif

	return success;
}


// --- Private: resize/move partition ---

bool GParted_Core::resize_move(const Partition& partition_old,
                               Partition& partition_new,
                               OperationDetail& operationdetail)
{
	bool success = true;

	if (partition_old.sector_start != partition_new.sector_start ||
	    partition_old.sector_end != partition_new.sector_end)
	{
		operationdetail.add_child(OperationDetail(_("resize/move partition")));

		success = parted_cmd().resizepart(partition_old.device_path,
		                                  partition_old.partition_number,
		                                  partition_new.sector_end);

		if (success && partition_old.sector_start != partition_new.sector_start)
		{
			success = parted_cmd().move(partition_old.device_path,
			                            partition_old.partition_number,
			                            partition_new.sector_start,
			                            partition_new.sector_end);
		}
	}

	return success;
}


// --- Private: name partition ---

bool GParted_Core::name_partition(const Partition& partition, OperationDetail& operationdetail)
{
	operationdetail.add_child(OperationDetail(
		Glib::ustring::compose(_("set partition name on %1"), partition.name)));

	return parted_cmd().name_partition(partition.device_path,
	                                   partition.partition_number,
	                                   partition.name);
}


// --- Private: calibrate ---

bool GParted_Core::calibrate_partition(Partition& partition, OperationDetail& operationdetail)
{
	if (partition.get_path().empty() || partition.partition_number < 1)
	{
		Glib::ustring output = parted_cmd().print_device(partition.device_path, "s");

		std::stringstream ss(output.raw());
		std::string line;
		bool in_table = false;

		while (std::getline(ss, line))
		{
			Glib::ustring ustr(line);

			if (ustr.find("Number") == 0)
			{
				in_table = true;
				continue;
			}

			if (in_table && !ustr.empty())
			{
				std::stringstream ls(ustr.raw());
				std::string token;
				ls >> token;
				int num = std::atoi(token.c_str());
				if (num > 0)
				{
					partition.partition_number = num;
					partition.set_path(partition.device_path +
					                   Utils::num_to_str(num));
					break;
				}
			}
		}
	}

	return true;
}


// --- Private: format / filesystem ---

bool GParted_Core::create_filesystem(const Partition& partition, OperationDetail& operationdetail)
{
	if (partition.fstype == FS_EXTENDED || partition.fstype == FS_UNALLOCATED)
		return true;

	FileSystem* p_filesystem = get_filesystem_object(partition.fstype);
	if (p_filesystem == nullptr)
	{
		return false;
	}

	return p_filesystem->create(partition, operationdetail);
}

bool GParted_Core::format(const Partition& partition, OperationDetail& operationdetail)
{
	if (partition.fstype == FS_EXTENDED || partition.fstype == FS_UNALLOCATED)
		return true;

	FileSystem* p_filesystem = get_filesystem_object(partition.fstype);
	if (p_filesystem == nullptr)
	{
		return false;
	}

	return p_filesystem->create(partition, operationdetail);
}

bool GParted_Core::remove_filesystem(const Partition& partition, OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::label_filesystem(const Partition& partition, OperationDetail& operationdetail)
{
	FileSystem* p_filesystem = get_filesystem_object(partition.fstype);
	if (p_filesystem == nullptr)
		return true;

	return p_filesystem->write_label(partition, operationdetail);
}

bool GParted_Core::change_filesystem_uuid(const Partition& partition, OperationDetail& operationdetail)
{
	FileSystem* p_filesystem = get_filesystem_object(partition.fstype);
	if (p_filesystem == nullptr)
		return true;

	return p_filesystem->write_uuid(partition, operationdetail);
}


// --- Private: copy ---

bool GParted_Core::copy(const Partition& partition_src,
                        Partition& partition_dst,
                        OperationDetail& operationdetail)
{
	operationdetail.add_child(OperationDetail(_("copy partition")));

	bool success = true;

	if (partition_src.fstype != FS_UNALLOCATED && partition_dst.fstype != FS_UNALLOCATED)
	{
		success = copy_filesystem(partition_src, partition_dst, operationdetail);
	}

	return success;
}

bool GParted_Core::copy_filesystem(const Partition& partition_src,
                                   Partition& partition_dst,
                                   OperationDetail& operationdetail)
{
	FileSystem* p_filesystem = get_filesystem_object(partition_src.fstype);
	if (p_filesystem == nullptr)
	{
		return false;
	}

	return p_filesystem->copy(partition_src, partition_dst, operationdetail);
}


// --- Private: settle ---

void GParted_Core::settle_device(std::time_t timeout)
{
	Glib::ustring cmd = "sleep " + Utils::num_to_str(timeout);
	adb().shell_command(cmd);
}


// --- Private: erase signatures ---

bool GParted_Core::erase_filesystem_signatures(const Partition& partition,
                                               OperationDetail& operationdetail)
{
	return true;
}


// --- Private: utility methods (stubs for ADB backend) ---

#ifndef USE_ADB_BACKEND

bool GParted_Core::get_device(const Glib::ustring& device_path, PedDevice*& lp_device, bool flush)
{
	lp_device = nullptr;
	return true;
}

bool GParted_Core::get_disk(PedDevice* lp_device, PedDisk*& lp_disk)
{
	lp_disk = nullptr;
	return true;
}

bool GParted_Core::get_device_and_disk(const Glib::ustring& device_path,
                                       PedDevice*& lp_device, PedDisk*& lp_disk)
{
	lp_device = nullptr;
	lp_disk = nullptr;
	return true;
}

void GParted_Core::destroy_device_and_disk(PedDevice*& lp_device, PedDisk*& lp_disk)
{
	lp_device = nullptr;
	lp_disk = nullptr;
}

bool GParted_Core::commit(PedDisk* lp_disk)
{
	return true;
}

bool GParted_Core::commit_to_os(PedDisk* lp_disk, std::time_t timeout)
{
	return true;
}

bool GParted_Core::useable_device(const PedDevice* lp_device)
{
	return true;
}

PedPartition* GParted_Core::get_lp_partition(const PedDisk* lp_disk, const Partition& partition)
{
	return nullptr;
}

bool GParted_Core::flush_device(PedDevice* lp_device)
{
	return true;
}

#endif

void GParted_Core::capture_libparted_messages(OperationDetail& operationdetail, bool success)
{
}

#ifndef USE_ADB_BACKEND
void GParted_Core::set_luks_partition(PartitionLUKS& partition)
{
}
#endif

void GParted_Core::set_partition_label_and_uuid(Partition& partition)
{
}

FSType GParted_Core::detect_filesystem_in_encryption_mapping(const Glib::ustring& path,
                                                             std::vector<Glib::ustring>& messages)
{
	return FS_UNKNOWN;
}

FSType GParted_Core::detect_filesystem_internal(const Glib::ustring& path, Byte_Value sector_size)
{
	return FS_UNKNOWN;
}

#ifndef USE_ADB_BACKEND
FSType GParted_Core::detect_filesystem(const PedDevice* lp_device, const PedPartition* lp_partition,
                                       std::vector<Glib::ustring>& messages)
{
	return FS_UNKNOWN;
}
#endif

void GParted_Core::read_label(Partition& partition)
{
}

void GParted_Core::read_uuid(Partition& partition)
{
}

void GParted_Core::set_mountpoints(Partition& partition)
{
}

bool GParted_Core::set_mountpoints_helper(Partition& partition, const Glib::ustring& path)
{
	return false;
}

bool GParted_Core::is_busy(const Glib::ustring& device_path, FSType fstype,
                           const Glib::ustring& partition_path)
{
	return false;
}

#ifdef USE_ADB_BACKEND
void GParted_Core::set_used_sectors(Partition& partition, PedDisk* lp_disk)
{
	log_set(("[set_used_sectors] partition=" + partition.get_path() + " type=" + Utils::num_to_str(partition.type) + " fstype=" + Utils::num_to_str(partition.fstype) + " sector_size=" + Utils::num_to_str(partition.sector_size)).c_str());

	if (partition.type == TYPE_UNALLOCATED || partition.type == TYPE_EXTENDED)
	{
		log_set("[set_used_sectors] skip: unallocated or extended");
		return;
	}

	if (partition.fstype == FS_UNKNOWN || partition.fstype == FS_UNALLOCATED)
	{
		log_set("[set_used_sectors] skip: unknown or unallocated fstype");
		return;
	}

	Glib::ustring part_path = partition.get_path();
	if (part_path.empty())
	{
		log_set("[set_used_sectors] skip: empty path");
		return;
	}

	const std::vector<Glib::ustring>& mountpoints = partition.get_mountpoints();
	Glib::ustring mountpoint;
	bool is_mounted = !mountpoints.empty();

	if (is_mounted)
		mountpoint = mountpoints[0];

	if (is_mounted && !mountpoint.empty())
	{
		log_set(("[set_used_sectors] mounted at " + mountpoint + ", using df").c_str());

		Glib::ustring df_cmd = "df " + Glib::shell_quote(mountpoint);
		Glib::ustring df_output;
		Glib::ustring df_error;
		adb().shell_command_exit_status(df_cmd, df_output, df_error);

		std::stringstream dss(df_output.raw());
		std::string dline;
		std::getline(dss, dline);
		if (std::getline(dss, dline))
		{
			std::stringstream dtokens(dline);
			std::string fs_name, total, used, avail;
			dtokens >> fs_name >> total >> used >> avail;

			long long total_1k = std::atoll(total.c_str());
			long long avail_1k = std::atoll(avail.c_str());
			long long total_bytes = total_1k * 1024;
			long long avail_bytes = avail_1k * 1024;
			long long used_bytes = total_bytes - avail_bytes;

			log_set(("[set_used_sectors] df: total=" + Utils::num_to_str(total_bytes) + " avail=" + Utils::num_to_str(avail_bytes) + " used=" + Utils::num_to_str(used_bytes)).c_str());

			if (total_bytes > 0 && partition.sector_size > 0)
			{
				partition.set_sector_usage(total_bytes / partition.sector_size,
				                           avail_bytes / partition.sector_size);
				log_set("[set_used_sectors] set_sector_usage done via df");
				return;
			}
		}
	}
	else
	{
		log_set(("[set_used_sectors] not mounted, is_mounted=" + Glib::ustring(is_mounted ? "true" : "false") + " mountpoint=[" + mountpoint + "]").c_str());
	}

	if (partition.fstype == FS_EXT2 || partition.fstype == FS_EXT3 || partition.fstype == FS_EXT4)
	{
		Glib::ustring tune_cmd = "tune2fs -l " + Glib::shell_quote(part_path);
		log_set(("[set_used_sectors] trying tune2fs: " + tune_cmd).c_str());
		Glib::ustring tune_output;
		Glib::ustring tune_error;
		int rc = adb().shell_command_exit_status(tune_cmd, tune_output, tune_error);
		log_set(("[set_used_sectors] tune2fs rc=" + Utils::num_to_str(rc) + " output_len=" + Utils::num_to_str(tune_output.size())).c_str());

		if (rc == 0)
		{
			long long block_count = 0;
			long long block_size = 0;
			long long free_blocks = 0;

			std::stringstream tss(tune_output.raw());
			std::string tline;
			while (std::getline(tss, tline))
			{
				Glib::ustring tu(tline);
				if (tu.find("Block count:") != Glib::ustring::npos)
				{
					size_t pos = tu.find(':');
					block_count = std::atoll(Utils::trim(tu.substr(pos + 1)).c_str());
				}
				else if (tu.find("Block size:") != Glib::ustring::npos)
				{
					size_t pos = tu.find(':');
					block_size = std::atoll(Utils::trim(tu.substr(pos + 1)).c_str());
				}
				else if (tu.find("Free blocks:") != Glib::ustring::npos)
				{
					size_t pos = tu.find(':');
					free_blocks = std::atoll(Utils::trim(tu.substr(pos + 1)).c_str());
				}
			}

			log_set(("[set_used_sectors] tune2fs: block_count=" + Utils::num_to_str(block_count) + " block_size=" + Utils::num_to_str(block_size) + " free_blocks=" + Utils::num_to_str(free_blocks) + " sector_size=" + Utils::num_to_str(partition.sector_size)).c_str());

			if (block_count > 0 && block_size > 0 && partition.sector_size > 0)
			{
				long long total_bytes = block_count * block_size;
				long long free_bytes = free_blocks * block_size;

				partition.set_sector_usage(total_bytes / partition.sector_size,
				                           free_bytes / partition.sector_size);
				log_set("[set_used_sectors] set_sector_usage done via tune2fs");
			}
			else
			{
				log_set("[set_used_sectors] tune2fs: invalid values, skipping");
			}
		}
	}
	else
	{
		log_set("[set_used_sectors] not ext2/3/4, no tune2fs");
	}
}
#endif

void GParted_Core::mounted_fs_set_used_sectors(Partition& partition)
{
}

#ifndef USE_ADB_BACKEND
void GParted_Core::set_flags(Partition& partition, PedPartition* lp_partition)
{
}
#endif

bool GParted_Core::move(const Partition& partition_old,
                        const Partition& partition_new,
                        OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::move_filesystem(const Partition& partition_old,
                                   const Partition& partition_new,
                                   OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::resize(const Partition& partition_old,
                          const Partition& partition_new,
                          OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::resize_encryption(const Partition& partition_old,
                                     const Partition& partition_new,
                                     OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::resize_plain(const Partition& partition_old,
                                const Partition& partition_new,
                                OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::resize_move_partition(const Partition& partition_old,
                                         const Partition& partition_new,
                                         OperationDetail& operationdetail,
                                         bool rollback_on_fail)
{
	return true;
}

bool GParted_Core::resize_move_partition_implement(const Partition& partition_old,
                                                   const Partition& partition_new,
                                                   Sector& new_start, Sector& new_end)
{
	return true;
}

bool GParted_Core::shrink_encryption(const Partition& partition_old,
                                     const Partition& partition_new,
                                     OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::maximize_encryption(const Partition& partition,
                                       OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::shrink_filesystem(const Partition& partition_old,
                                     const Partition& partition_new,
                                     OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::maximize_filesystem(const Partition& partition, OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::recreate_linux_swap_filesystem(const Partition& partition,
                                                  OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::resize_filesystem_implement(const Partition& partition_old,
                                               const Partition& partition_new,
                                               OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::copy_filesystem_internal(const Partition& partition_src,
                                            const Partition& partition_dst,
                                            OperationDetail& operationdetail,
                                            bool cancel_safe)
{
	return true;
}

bool GParted_Core::copy_filesystem_internal(const Partition& partition_src,
                                            const Partition& partition_dst,
                                            OperationDetail& operationdetail,
                                            Byte_Value& total_done, bool cancel_safe)
{
	return true;
}

bool GParted_Core::copy_blocks(const Glib::ustring& src_device, const Glib::ustring& dst_device,
                               Sector src_start, Sector dst_start,
                               Byte_Value src_sector_size, Byte_Value dst_sector_size,
                               Byte_Value src_length, OperationDetail& operationdetail,
                               Byte_Value& total_done, bool cancel_safe)
{
	return true;
}

void GParted_Core::rollback_move_filesystem(const Partition& partition_src,
                                            const Partition& partition_dst,
                                            OperationDetail& operationdetail,
                                            Byte_Value total_done)
{
}

bool GParted_Core::check_repair_filesystem(const Partition& partition, OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::check_repair_maximize(const Partition& partition, OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::set_partition_type(const Partition& partition, OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::calculate_exact_geom(const Partition& partition_old,
                                        Partition& partition_new,
                                        OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::update_dmraid_entry(const Partition& partition_new, OperationDetail& operationdetail)
{
	return true;
}

bool GParted_Core::update_bootsector(const Partition& partition, OperationDetail& operationdetail)
{
	return true;
}

#if defined(HAVE_LIBPARTED_FS_RESIZE) && !defined(USE_ADB_BACKEND)
void GParted_Core::LP_set_used_sectors(Partition& partition, PedDisk* lp_disk)
{
}

bool GParted_Core::resize_move_filesystem_using_libparted(const Partition& partition_old,
                                                          const Partition& partition_new,
                                                          OperationDetail& operationdetail)
{
	return true;
}

void GParted_Core::thread_lp_ped_file_system_resize(PedFileSystem* fs, PedGeometry* lp_geom,
                                                    bool* return_value)
{
}
#endif

#ifndef USE_ADB_BACKEND
Glib::ustring GParted_Core::get_partition_path(const PedPartition* lp_partition)
{
	return "";
}

void GParted_Core::set_device_one_partition(Device& device, PedDevice* lp_device, FSType fstype,
                                            std::vector<Glib::ustring>& messages)
{
}

void GParted_Core::set_device_partitions(Device& device, PedDevice* lp_device, PedDisk* lp_disk)
{
}

bool GParted_Core::set_partition_type_using_flag(PedPartition* lp_partition, PedPartitionFlag flag, PedPartitionFlag other_flag)
{
	return true;
}

bool GParted_Core::set_partition_flag(PedPartition* lp_partition, const Partition& partition,
                                      const Glib::ustring& checked_flag, const Glib::ustring& flag_to_set, bool check_state)
{
	return true;
}

bool GParted_Core::set_partition_type_using_fstype(PedPartition* lp_partition, const Glib::ustring& fstype)
{
	return true;
}
#endif // USE_ADB_BACKEND

std::unique_ptr<SupportedFileSystems> GParted_Core::supported_filesystems;

} // namespace GParted

#endif // USE_ADB_BACKEND