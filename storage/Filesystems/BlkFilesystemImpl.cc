/*
 * Copyright (c) [2014-2015] Novell, Inc.
 * Copyright (c) [2016-2017] SUSE LLC
 *
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, contact Novell, Inc.
 *
 * To contact Novell about this file by physical or electronic mail, you may
 * find current contact information at www.novell.com.
 */


#include <glob.h>
#include <iostream>
#include <boost/algorithm/string.hpp>

#include "storage/Utils/XmlFile.h"
#include "storage/Utils/Enum.h"
#include "storage/Utils/StorageTmpl.h"
#include "storage/Utils/StorageDefines.h"
#include "storage/Utils/SystemCmd.h"
#include "storage/Utils/HumanString.h"
#include "storage/Filesystems/BlkFilesystemImpl.h"
#include "storage/Filesystems/MountPointImpl.h"
#include "storage/Devices/BlkDeviceImpl.h"
#include "storage/Devices/LvmLv.h"
#include "storage/Devicegraph.h"
#include "storage/SystemInfo/SystemInfo.h"
#include "storage/StorageImpl.h"
#include "storage/FreeInfo.h"
#include "storage/Prober.h"
#include "storage/Redirect.h"


namespace storage
{

    using namespace std;


    const char* DeviceTraits<BlkFilesystem>::classname = "BlkFilesystem";


    BlkFilesystem::Impl::Impl(const xmlNode* node)
	: Filesystem::Impl(node), label(), uuid(), mkfs_options(), tune_options(), resize_info(),
	  content_info()
    {
	getChildValue(node, "label", label);
	getChildValue(node, "uuid", uuid);

	getChildValue(node, "mkfs-options", mkfs_options);
	getChildValue(node, "tune-options", tune_options);

	const xmlNode* resize_info_node = getChildNode(node, "ResizeInfo");
	if (resize_info_node)
	    resize_info.set_value(ResizeInfo(resize_info_node));

	const xmlNode* content_info_node = getChildNode(node, "ContentInfo");
	if (content_info_node)
	    content_info.set_value(ContentInfo(content_info_node));
    }


    void
    BlkFilesystem::Impl::set_label(const string& label)
    {
	Impl::label = label;
    }


    void
    BlkFilesystem::Impl::set_uuid(const string& uuid)
    {
	Impl::uuid = uuid;
    }


    void
    BlkFilesystem::Impl::set_mkfs_options(const string& mkfs_options)
    {
	Impl::mkfs_options = mkfs_options;
    }


    void
    BlkFilesystem::Impl::set_tune_options(const string& tune_options)
    {
	Impl::tune_options = tune_options;
    }


    MountByType
    BlkFilesystem::Impl::get_default_mount_by() const
    {
	const BlkDevice* blk_device = get_blk_device();

	if (is_lvm_lv(blk_device))
	    return MountByType::DEVICE;
	else
	    return get_storage()->get_default_mount_by();
    }


    void
    BlkFilesystem::Impl::save(xmlNode* node) const
    {
	Filesystem::Impl::save(node);

	setChildValueIf(node, "label", label, !label.empty());
	setChildValueIf(node, "uuid", uuid, !uuid.empty());

	setChildValueIf(node, "mkfs-options", mkfs_options, !mkfs_options.empty());
	setChildValueIf(node, "tune-options", tune_options, !tune_options.empty());

	if (resize_info.has_value())
	{
	    xmlNode* resize_info_node = xmlNewChild(node, "ResizeInfo");
	    resize_info.get_value().save(resize_info_node);
	}

	if (content_info.has_value())
	{
	    xmlNode* content_info_node = xmlNewChild(node, "ContentInfo");
	    content_info.get_value().save(content_info_node);
	}
    }


    void
    BlkFilesystem::Impl::probe_pass_2a(Prober& prober)
    {
	SystemInfo& system_info = prober.get_system_info();

	const BlkDevice* blk_device = get_blk_device();

	const Blkid& blkid = system_info.getBlkid();
	Blkid::const_iterator it = blkid.find_by_name(blk_device->get_name(), system_info);
	if (it != blkid.end())
	{
	    label = it->second.fs_label;
	    uuid = it->second.fs_uuid;
	}
    }


    void
    BlkFilesystem::Impl::probe_pass_2b(Prober& prober)
    {
	SystemInfo& system_info = prober.get_system_info();

	const BlkDevice* blk_device = get_blk_device();

	vector<string> aliases = EtcFstab::construct_device_aliases(blk_device, get_non_impl());

	vector<const FstabEntry*> fstab_entries = find_etc_fstab_entries(system_info.getEtcFstab(), aliases);
	vector<const FstabEntry*> mount_entries = find_proc_mounts_entries(system_info, aliases);

	// The code here works only with one mount point per
	// mountable. Anything else is not supported since rejected by the
	// product owner.

	vector<JointEntry> joint_entries = join_entries(fstab_entries, mount_entries);
	if (!joint_entries.empty())
	    joint_entries[0].add_to(get_non_impl());
    }


    void
    BlkFilesystem::Impl::probe_uuid()
    {
	const BlkDevice* blk_device = get_blk_device();

	const Blkid& blkid(blk_device->get_name());
	Blkid::const_iterator it = blkid.get_sole_entry();
	if (it != blkid.end())
	    uuid = it->second.fs_uuid;
    }


    ResizeInfo
    BlkFilesystem::Impl::detect_resize_info() const
    {
	/*
	 * If the filesystem is not on-disk, ResizeInfo(true, min_size,
	 * max_size) is returned.
	 *
	 * Otherwise detect_resize_info_on_disk() is called (on the probed
	 * devicegraph), the value cached and returned.
	 *
	 * The on-disk ResizeInfo is cached since it is invariant. E.g. the
	 * ResizeInfo of a partition changes when the following partition is
	 * removed.
	 *
	 * Filesystems that do not support create nor resize, e.g. ISO9660,
	 * override detect_resize_info() and just return ResizeInfo(false).
	 *
	 * Filesystems can also override detect_resize_info_on_disk() if they
	 * need special handling to detect the values, e.g. Ntfs calls a
	 * external program to do so. In general a tools for every filesystem
	 * reporting min and max size would be nice (LOL).
	 */

	if (!exists_in_probed())
	{
	    return ResizeInfo(true, min_size(), max_size());
	}

	if (!resize_info.has_value())
	{
	    const BlkFilesystem* tmp_blk_filesystem = redirect_to_probed(get_non_impl());

	    ResizeInfo tmp_resize_info = tmp_blk_filesystem->get_impl().detect_resize_info_on_disk();

	    y2mil("on-disk resize-info:" << tmp_resize_info);

	    resize_info.set_value(tmp_resize_info);
	}

	return resize_info.get_value();
    }


    ResizeInfo
    BlkFilesystem::Impl::detect_resize_info_on_disk() const
    {
	// TODO only in real probe mode allowed

	if (!get_devicegraph()->get_impl().is_probed())
	    ST_THROW(Exception("function called on wrong device"));

	ResizeInfo resize_info(true, min_size(), max_size());

	unsigned long long blk_device_size = get_blk_device()->get_size();

	// Even if shrink or grow is not supported it is still possible to
	// shrink or grow to the original size.

	if (!supports_shrink())
	{
	    resize_info.min_size = blk_device_size;
	}
	else
	{
	    // Assume that the min-size, e.g. for superblocks, metadata and
	    // journal, is needed additional to the used-size.

	    resize_info.min_size += used_size_on_disk();

	    // But the min-size must never be bigger than the blk device size.

	    resize_info.min_size = min(resize_info.min_size, blk_device_size);
	}

	if (!supports_grow())
	{
	    resize_info.max_size = blk_device_size;
	}

	resize_info.check();

	return resize_info;
    }


    unsigned long long
    BlkFilesystem::Impl::used_size_on_disk() const
    {
	// TODO only in real probe mode allowed

        EnsureMounted ensure_mounted(get_filesystem());

        StatVfs stat_vfs = detect_stat_vfs(ensure_mounted.get_any_mount_point());

        return stat_vfs.size - stat_vfs.free;
    }


    void
    BlkFilesystem::Impl::set_resize_info(const ResizeInfo& tmp)
    {
	resize_info.set_value(tmp);
    }


    ContentInfo
    BlkFilesystem::Impl::detect_content_info() const
    {
	if (!content_info.has_value())
	{
	    content_info.set_value(detect_content_info_on_disk());
	}

	return content_info.get_value();
    }


    ContentInfo
    BlkFilesystem::Impl::detect_content_info_on_disk() const
    {
	// TODO only in real probe mode allowed

	EnsureMounted ensure_mounted(get_filesystem());

	ContentInfo content_info;
	content_info.is_windows = false;
	content_info.is_efi = false;
	content_info.num_homes = detect_num_homes(ensure_mounted.get_any_mount_point());

	return content_info;
    }


    void
    BlkFilesystem::Impl::set_content_info(const ContentInfo& tmp)
    {
	content_info.set_value(tmp);
    }


    bool
    BlkFilesystem::Impl::detect_is_windows(const string& mountpoint)
    {
	// The file '$Boot' is special. It is a reserved name of NTFS and from
	// linux not visible with 'ls /mnt' but with 'ls /mnt/$Boot'.

	const char* files[] = { "boot.ini", "msdos.sys", "io.sys", "config.sys", "MSDOS.SYS",
				"IO.SYS", "bootmgr", "$Boot" };

	for (unsigned int i = 0; i < lengthof(files); ++i)
	{
	    string file = mountpoint + "/" + files[i];
	    if (access(file.c_str(), R_OK) == 0)
	    {
		y2mil("found windows file " << quote(file));
		return true;
	    }
	}

	return false;
    }


    bool
    BlkFilesystem::Impl::detect_is_efi(const string& mountpoint)
    {
	return checkDir(mountpoint + "/efi");
    }


    unsigned
    BlkFilesystem::Impl::detect_num_homes(const string& mountpoint)
    {
	const char* files[] = { ".profile", ".bashrc", ".ssh", ".kde", ".kde4", ".gnome",
				".gnome2" };

	unsigned num_homes = 0;

	const vector<string> dirs = glob(mountpoint + "/*", GLOB_NOSORT | GLOB_ONLYDIR);
	for (const string& dir : dirs)
	{
	    if (!boost::ends_with(dir, "/root") && checkDir(dir))
	    {
		for (unsigned int i = 0; i < lengthof(files); ++i)
		{
		    string file = dir + "/" + files[i];
		    if (access(file.c_str(), R_OK) == 0)
		    {
			y2mil("found home file " << quote(file));
			++num_homes;
			break;
		    }
		}
	    }

	    if (num_homes >= 2)
		return num_homes;
	}

	return num_homes;
    }


    void
    BlkFilesystem::Impl::add_create_actions(Actiongraph::Impl& actiongraph) const
    {
	vector<Action::Base*> actions;

	actions.push_back(new Action::Create(get_sid()));

	if (!get_label().empty())
	    actions.push_back(new Action::SetLabel(get_sid()));

	if (!get_uuid().empty())
	    actions.push_back(new Action::SetUuid(get_sid()));

	if (!tune_options.empty())
	    actions.push_back(new Action::SetTuneOptions(get_sid()));

	actiongraph.add_chain(actions);
    }


    void
    BlkFilesystem::Impl::add_modify_actions(Actiongraph::Impl& actiongraph, const Device* lhs_base) const
    {
	const Impl& lhs = dynamic_cast<const Impl&>(lhs_base->get_impl());

	vector<Action::Base*> actions;

	if (get_type() != lhs.get_type())
	{
	    ST_THROW(Exception("cannot change filesystem type"));
	}

	if (get_label() != lhs.get_label())
	{
	    actions.push_back(new Action::SetLabel(get_sid()));
	}

	if (get_uuid() != lhs.get_uuid())
	{
	    actions.push_back(new Action::SetUuid(get_sid()));
	}

	if (get_tune_options() != lhs.get_tune_options())
	{
	    actions.push_back(new Action::SetTuneOptions(get_sid()));
	}

	// TODO depends on mount-by, whether there actually is an entry in fstab,
	// multiple devices, ...
	if (get_blk_device()->get_name() != lhs.get_blk_device()->get_name())
	{
	    if (has_mount_point())
		actions.push_back(new Action::RenameInEtcFstab(get_sid()));
	}

	actiongraph.add_chain(actions);
    }


    vector<const BlkDevice*>
    BlkFilesystem::Impl::get_blk_devices() const
    {
	const Devicegraph* devicegraph = get_devicegraph();
	Devicegraph::Impl::vertex_descriptor vertex = get_vertex();

	return devicegraph->get_impl().filter_devices_of_type<BlkDevice>(devicegraph->get_impl().parents(vertex));
    }


    const BlkDevice*
    BlkFilesystem::Impl::get_blk_device() const
    {
	vector<const BlkDevice*> blk_devices = get_blk_devices();
	if (blk_devices.size() != 1)
	    throw;

	return blk_devices.front();
    }


    void
    BlkFilesystem::Impl::wait_for_devices() const
    {
	storage::wait_for_devices(get_blk_devices());
    }


    bool
    BlkFilesystem::Impl::equal(const Device::Impl& rhs_base) const
    {
	const Impl& rhs = dynamic_cast<const Impl&>(rhs_base);

	if (!Filesystem::Impl::equal(rhs))
	    return false;

	return label == rhs.label && uuid == rhs.uuid && mkfs_options == rhs.mkfs_options &&
	    tune_options == rhs.tune_options;
    }


    void
    BlkFilesystem::Impl::log_diff(std::ostream& log, const Device::Impl& rhs_base) const
    {
	const Impl& rhs = dynamic_cast<const Impl&>(rhs_base);

	Filesystem::Impl::log_diff(log, rhs);

	storage::log_diff(log, "label", label, rhs.label);
	storage::log_diff(log, "uuid", uuid, rhs.uuid);

	storage::log_diff(log, "mkfs-options", mkfs_options, rhs.mkfs_options);
	storage::log_diff(log, "tune-options", tune_options, rhs.tune_options);
    }


    void
    BlkFilesystem::Impl::print(std::ostream& out) const
    {
	Filesystem::Impl::print(out);

	if (!label.empty())
	    out << " label:" << label;

	if (!uuid.empty())
	    out << " uuid:" << uuid;

	if (!mkfs_options.empty())
	    out << " mkfs-options:" << mkfs_options;

	if (!tune_options.empty())
	    out << " tune-options:" << tune_options;
    }


    string
    BlkFilesystem::Impl::get_mount_name() const
    {
	const BlkDevice* blk_device = get_blk_device();

	return blk_device->get_name();
    }


    string
    BlkFilesystem::Impl::get_mount_by_name(MountByType mount_by_type) const
    {
	string ret;

	switch (mount_by_type)
	{
	    case MountByType::UUID:
		if (!uuid.empty())
		    ret = "UUID=" + uuid;
		else
		    y2err("no uuid defined, using fallback");
		break;

	    case MountByType::LABEL:
		if (!label.empty())
		    ret = "LABEL=" + label;
		else
		    y2err("no label defined, using fallback");
		break;

	    case MountByType::ID:
	    case MountByType::PATH:
	    case MountByType::DEVICE:
		break;
	}

	if (ret.empty())
	{
	    const BlkDevice* blk_device = get_blk_device();

	    ret = blk_device->get_impl().get_mount_by_name(mount_by_type);
	}

	return ret;
    }


    Text
    BlkFilesystem::Impl::do_create_text(Tense tense) const
    {
	// TODO handle multiple BlkDevices

	const BlkDevice* blk_device = get_blk_device();

	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by file system name (e.g. ext4),
			   // %2$s is replaced by device name (e.g. /dev/sda1),
			   // %3$s is replaced by size (e.g. 2GiB)
			   _("Create %1$s on %2$s (%3$s)"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by file system name (e.g. ext4),
			   // %2$s is replaced by device name (e.g. /dev/sda1),
			   // %3$s is replaced by size (e.g. 2GiB)
			   _("Creating %1$s on %2$s (%3$s)"));

	return sformat(text, get_displayname().c_str(), blk_device->get_name().c_str(),
		       blk_device->get_size_string().c_str());
    }


    Text
    BlkFilesystem::Impl::do_set_label_text(Tense tense) const
    {
	const BlkDevice* blk_device = get_blk_device();

	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by device name (e.g. /dev/sda1),
			   // %2$s is replaced by label (e.g. ROOT)
			   _("Set label of %1$s to %2$s"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by device name (e.g. /dev/sda1),
			   // %2$s is replaced by label (e.g. ROOT)
			   _("Setting label of %1$s to %2$s"));

	return sformat(text, blk_device->get_name().c_str(), label.c_str());
    }


    void
    BlkFilesystem::Impl::do_set_label() const
    {
	ST_THROW(LogicException("stub do_set_label called"));
    }


    Text
    BlkFilesystem::Impl::do_set_uuid_text(Tense tense) const
    {
	const BlkDevice* blk_device = get_blk_device();

	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by device name (e.g. /dev/sda1),
			   // %2$s is replaced by UUID (e.g. 3cfa63b5-4d29-43e6-8658-57b74f68fd7f)
			   _("Set UUID of %1$s to %2$s"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by device name (e.g. /dev/sda1),
			   // %2$s is replaced by UUID (e.g. 3cfa63b5-4d29-43e6-8658-57b74f68fd7f)
			   _("Setting UUID of %1$s to %2$s"));

	return sformat(text, blk_device->get_name().c_str(), uuid.c_str());
    }


    void
    BlkFilesystem::Impl::do_set_uuid() const
    {
	ST_THROW(LogicException("stub do_set_uuid called"));
    }


    Text
    BlkFilesystem::Impl::do_set_tune_options_text(Tense tense) const
    {
	const BlkDevice* blk_device = get_blk_device();

	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by device name (e.g. /dev/sda1)
			   _("Set tune options of %1$s"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by device name (e.g. /dev/sda1)
			   _("Setting tune options of %1$s"));

	return sformat(text, blk_device->get_name().c_str(), uuid.c_str());
    }


    void
    BlkFilesystem::Impl::do_set_tune_options() const
    {
	ST_THROW(LogicException("stub do_set_tune_options called"));
    }


    Text
    BlkFilesystem::Impl::do_rename_in_etc_fstab_text(const Device* lhs, Tense tense) const
    {
	const BlkDevice* blk_device_lhs = to_blk_filesystem(lhs)->get_impl().get_blk_device();
	const BlkDevice* blk_device_rhs = get_blk_device();

	const MountPoint* mount_point = get_mount_point();

	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by mount point (e.g. /home),
			   // %2$s is replaced by device name (e.g. /dev/sda6),
			   // %3$s is replaced by device name (e.g. /dev/sda5)
			   _("Rename mount point %1$s from %2$s to %3$s in /etc/fstab"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by mount point (e.g. /home),
			   // %2$s is replaced by device name (e.g. /dev/sda6),
			   // %3$s is replaced by device name (e.g. /dev/sda5)
			   _("Renaming mount point %1$s from %2$s to %3$s in /etc/fstab"));

	return sformat(text, mount_point->get_path().c_str(), blk_device_lhs->get_name().c_str(),
		       blk_device_rhs->get_name().c_str());
    }


    void
    BlkFilesystem::Impl::do_rename_in_etc_fstab(CommitData& commit_data, const Device* lhs) const
    {
	EtcFstab& etc_fstab = commit_data.get_etc_fstab();

	const MountPoint* mount_point = get_mount_point();

	for (FstabEntry* entry : find_etc_fstab_entries(etc_fstab, { mount_point->get_impl().get_fstab_device_name() }))
	{
	    entry->set_device(get_mount_by_name(mount_point->get_mount_by()));
	    etc_fstab.log_diff();
	    etc_fstab.write();
	}
    }


    Text
    BlkFilesystem::Impl::do_resize_text(ResizeMode resize_mode, const Device* lhs, const Device* rhs,
					Tense tense) const
    {
	const BlkDevice* blk_device = get_blk_device();

	const BlkDevice* blk_device_lhs = to_blk_filesystem(lhs)->get_impl().get_blk_device();
	const BlkDevice* blk_device_rhs = to_blk_filesystem(rhs)->get_impl().get_blk_device();

	Text text;

	switch (resize_mode)
	{
	    case ResizeMode::SHRINK:
		text = tenser(tense,
			      // TRANSLATORS: displayed before action,
			      // %1$s is replaced by file system (e.g. ext4),
			      // %2$s is replaced by device name (e.g. /dev/sda1),
			      // %3$s is replaced by old size (e.g. 2GiB),
			      // %4$s is replaced by new size (e.g. 1GiB)
			      _("Shrink %1$s on %2$s from %3$s to %4$s"),
			      // TRANSLATORS: displayed during action,
			      // %1$s is replaced by file system (e.g. ext4),
			      // %2$s is replaced by device name (e.g. /dev/sda1),
			      // %3$s is replaced by old size (e.g. 2GiB),
			      // %4$s is replaced by new size (e.g. 1GiB)
			      _("Shrinking %1$s on %2$s from %3$s to %4$s"));
		break;

	    case ResizeMode::GROW:
		text = tenser(tense,
			      // TRANSLATORS: displayed before action,
			      // %1$s is replaced by file system (e.g. ext4),
			      // %2$s is replaced by device name (e.g. /dev/sda1),
			      // %3$s is replaced by old size (e.g. 1GiB),
			      // %4$s is replaced by new size (e.g. 2GiB)
			      _("Grow %1$s on %2$s from %3$s to %4$s"),
			      // TRANSLATORS: displayed during action,
			      // %1$s is replaced by file system (e.g. ext4),
			      // %2$s is replaced by device name (e.g. /dev/sda1),
			      // %3$s is replaced by old size (e.g. 1GiB),
			      // %4$s is replaced by new size (e.g. 2GiB)
			      _("Growing %1$s on %2$s from %3$s to %4$s"));
		break;

	    default:
		ST_THROW(LogicException("invalid value for resize_mode"));
	}

	return sformat(text, get_displayname().c_str(), blk_device->get_name().c_str(),
		       blk_device_lhs->get_size_string().c_str(),
		       blk_device_rhs->get_size_string().c_str());
    }


    Text
    BlkFilesystem::Impl::do_delete_text(Tense tense) const
    {
	// TODO handle multiple BlkDevices

	const BlkDevice* blk_device = get_blk_device();

	Text text = tenser(tense,
			   // TRANSLATORS: displayed before action,
			   // %1$s is replaced by file system name (e.g. ext4),
			   // %2$s is replaced by device name (e.g. /dev/sda1),
			   // %3$s is replaced by size (e.g. 2GiB)
			   _("Delete %1$s on %2$s (%3$s)"),
			   // TRANSLATORS: displayed during action,
			   // %1$s is replaced by file system name (e.g. ext4),
			   // %2$s is replaced by device name (e.g. /dev/sda1),
			   // %3$s is replaced by size (e.g. 2GiB)
			   _("Deleting %1$s on %2$s (%3$s)"));

	return sformat(text, get_displayname().c_str(), blk_device->get_name().c_str(),
		       blk_device->get_size_string().c_str());
    }


    void
    BlkFilesystem::Impl::do_delete() const
    {
	for (const BlkDevice* blk_device : get_blk_devices())
	{
	    blk_device->get_impl().wipe_device();
	}
    }


    namespace Action
    {

	Text
	SetLabel::text(const CommitData& commit_data) const
	{
	    const BlkFilesystem* blk_filesystem = to_blk_filesystem(get_device(commit_data.actiongraph, RHS));
	    return blk_filesystem->get_impl().do_set_label_text(commit_data.tense);
	}


	void
	SetLabel::commit(CommitData& commit_data, const CommitOptions& commit_options) const
	{
	    const BlkFilesystem* blk_filesystem = to_blk_filesystem(get_device(commit_data.actiongraph, RHS));
	    blk_filesystem->get_impl().do_set_label();
	}


	Text
	SetUuid::text(const CommitData& commit_data) const
	{
	    const BlkFilesystem* blk_filesystem = to_blk_filesystem(get_device(commit_data.actiongraph, RHS));
	    return blk_filesystem->get_impl().do_set_uuid_text(commit_data.tense);
	}


	void
	SetUuid::commit(CommitData& commit_data, const CommitOptions& commit_options) const
	{
	    const BlkFilesystem* blk_filesystem = to_blk_filesystem(get_device(commit_data.actiongraph, RHS));
	    blk_filesystem->get_impl().do_set_uuid();
	}


	Text
	SetTuneOptions::text(const CommitData& commit_data) const
	{
	    const BlkFilesystem* blk_filesystem = to_blk_filesystem(get_device(commit_data.actiongraph, RHS));
	    return blk_filesystem->get_impl().do_set_tune_options_text(commit_data.tense);
	}


	void
	SetTuneOptions::commit(CommitData& commit_data, const CommitOptions& commit_options) const
	{
	    const BlkFilesystem* blk_filesystem = to_blk_filesystem(get_device(commit_data.actiongraph, RHS));
	    blk_filesystem->get_impl().do_set_tune_options();
	}


	Text
	RenameInEtcFstab::text(const CommitData& commit_data) const
	{
	    const BlkFilesystem* blk_filesystem_lhs = to_blk_filesystem(get_device(commit_data.actiongraph, LHS));
	    const BlkFilesystem* blk_filesystem_rhs = to_blk_filesystem(get_device(commit_data.actiongraph, RHS));
	    return blk_filesystem_rhs->get_impl().do_rename_in_etc_fstab_text(blk_filesystem_lhs, commit_data.tense);
	}


	void
	RenameInEtcFstab::commit(CommitData& commit_data, const CommitOptions& commit_options) const
	{
	    const BlkFilesystem* blk_filesystem_lhs = to_blk_filesystem(get_device(commit_data.actiongraph, LHS));
	    const BlkFilesystem* blk_filesystem_rhs = to_blk_filesystem(get_device(commit_data.actiongraph, RHS));
	    blk_filesystem_rhs->get_impl().do_rename_in_etc_fstab(commit_data, blk_filesystem_lhs);
	}


	const BlkDevice*
	RenameInEtcFstab::get_renamed_blk_device(const Actiongraph::Impl& actiongraph, Side side) const
	{
	    const BlkFilesystem* blk_filesystem = to_blk_filesystem(get_device(actiongraph, side));
	    vector<const BlkDevice*> blk_devices = blk_filesystem->get_blk_devices();
	    return blk_devices[0]; // TODO, filesystems with multiple devices
	}

    }

}
