#ifndef GPARTED_PARTED_PARSER_H
#define GPARTED_PARTED_PARSER_H

#include "adb/PartedCommand.h"
#include "GParted_Core.h"
#include "Device.h"
#include "Partition.h"
#include "Utils.h"

#include <glibmm/ustring.h>
#include <vector>
#include <map>

namespace GParted
{

class PartedParser
{
public:
	PartedParser();

	std::vector<Glib::ustring> parse_device_paths(const Glib::ustring& output);

	bool parse_device_info(const Glib::ustring& output, Device& device);

	bool parse_partitions(const Glib::ustring& output, Device& device);

	std::vector<Glib::ustring> parse_disk_label_types(const Glib::ustring& output);

	std::map<Glib::ustring, bool> parse_available_flags(const Glib::ustring& output, int partition_number);

	int parse_sector_size(const Glib::ustring& output);

	static Sector parse_parted_size(const Glib::ustring& size_str, int sector_size);

public:
	static FSType parse_fs_type(const Glib::ustring& fs_name);

private:
	static void split_line(const Glib::ustring& line, char delimiter, std::vector<Glib::ustring>& parts);

	static Glib::ustring trim(const Glib::ustring& str);
};

} // namespace GParted

#endif // GPARTED_PARTED_PARSER_H