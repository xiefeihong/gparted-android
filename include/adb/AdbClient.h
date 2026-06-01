#ifndef GPARTED_ADB_CLIENT_H
#define GPARTED_ADB_CLIENT_H

#include <glibmm/ustring.h>
#include <string>
#include <stdexcept>

namespace GParted
{

class AdbError : public std::runtime_error
{
public:
	explicit AdbError(const std::string& msg) : std::runtime_error(msg) {}
};

class AdbClient
{
public:
	AdbClient();
	explicit AdbClient(const Glib::ustring& device_serial);
	~AdbClient();

	void set_device_serial(const Glib::ustring& serial);
	const Glib::ustring& get_device_serial() const;

	bool is_device_connected();
	bool wait_for_device(int timeout_seconds = 10);

	Glib::ustring shell_command(const Glib::ustring& command);
	int shell_command_exit_status(const Glib::ustring& command, Glib::ustring& output, Glib::ustring& error);

	bool push_file(const Glib::ustring& local_path, const Glib::ustring& remote_path);
	bool pull_file(const Glib::ustring& remote_path, const Glib::ustring& local_path);

	bool is_root();
	bool remount_system();

private:
	Glib::ustring build_adb_command(const Glib::ustring& args) const;
	int raw_shell_command_exit_status(const Glib::ustring& command, Glib::ustring& output, Glib::ustring& error);
	void check_root();
	static Glib::ustring escape_single_quotes(const Glib::ustring& str);

	Glib::ustring m_device_serial;
	bool m_is_root;
	bool m_su_available;
	bool m_root_checked;
};

} // namespace GParted

#endif // GPARTED_ADB_CLIENT_H