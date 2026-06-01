#ifndef GPARTED_PARTED_COMMAND_H
#define GPARTED_PARTED_COMMAND_H

#include "adb/AdbClient.h"
#include "Utils.h"

#include <glibmm/ustring.h>
#include <vector>
#include <map>

namespace GParted
{

struct PDeviceInfo
{
	Glib::ustring path;
	Glib::ustring model;
	Sector length;
	int sector_size;
	int logical_sector_size;
	int physical_sector_size;
	Glib::ustring disk_type;
	std::vector<Glib::ustring> disk_flags;
};

struct PPartitionInfo
{
	int number;
	Sector start;
	Sector end;
	Sector size;
	Glib::ustring file_system;
	Glib::ustring name;
	std::vector<Glib::ustring> flags;
};

struct PDeviceFullInfo
{
	PDeviceInfo device;
	std::vector<PPartitionInfo> partitions;
};

class PartedCommand
{
public:
	PartedCommand(AdbClient& adb);

	Glib::ustring list_devices();
	Glib::ustring print_device(const Glib::ustring& device_path, const Glib::ustring& unit = "s");
	Glib::ustring print_all(const Glib::ustring& unit = "s");

	bool mklabel(const Glib::ustring& device_path, const Glib::ustring& label_type);
	bool mkpart(const Glib::ustring& device_path, const Glib::ustring& part_type,
	            const Glib::ustring& fs_type, Sector start, Sector end);
	bool rm(const Glib::ustring& device_path, int partition_number);
	bool resizepart(const Glib::ustring& device_path, int partition_number, Sector end);
	bool set_flag(const Glib::ustring& device_path, int partition_number, const Glib::ustring& flag, bool state);
	bool name_partition(const Glib::ustring& device_path, int partition_number, const Glib::ustring& name);
	bool move(const Glib::ustring& device_path, int partition_number, Sector start, Sector end);

	Glib::ustring get_last_error() const;

	static Glib::ustring sector_to_parted_size(Sector sectors, int sector_size);

private:
	Glib::ustring parted_cmd(const Glib::ustring& args);

	AdbClient& m_adb;
	Glib::ustring m_last_error;
};

} // namespace GParted

#endif // GPARTED_PARTED_COMMAND_H