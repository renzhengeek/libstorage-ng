

#include "storage/Devices/DiskImpl.h"
#include "storage/Devices/Msdos.h"
#include "storage/Devices/Gpt.h"
#include "storage/Holders/Using.h"
#include "storage/Devicegraph.h"
#include "storage/Action.h"
#include "storage/Storage.h"
#include "storage/Environment.h"
#include "storage/SystemInfo/SystemInfo.h"
#include "storage/Utils/Enum.h"
#include "storage/Utils/StorageTmpl.h"
#include "storage/Utils/StorageTypes.h"
#include "storage/Utils/StorageDefines.h"


namespace storage
{

    using namespace std;


    Disk::Impl::Impl(const xmlNode* node)
	: BlkDevice::Impl(node), rotational(false), transport(TUNKNOWN)
    {
	string tmp;

	getChildValue(node, "geometry", geometry);

	getChildValue(node, "rotational", rotational);

	if (getChildValue(node, "transport", tmp))
	    transport = toValueWithFallback(tmp, TUNKNOWN);
    }


    vector<string>
    Disk::Impl::probe_disks(SystemInfo& systeminfo)
    {
	vector<string> ret;

	for (const string& name : systeminfo.getDir(SYSFSDIR "/block"))
	{
	    // we do not treat mds as disks although they can be partitioned since kernel 2.6.28
	    if (boost::starts_with(name, "md") || boost::starts_with(name, "loop"))
		continue;

	    const CmdUdevadmInfo udevadminfo = systeminfo.getCmdUdevadmInfo("/dev/" + name);

	    const File range_file = systeminfo.getFile(SYSFSDIR + udevadminfo.get_path() + "/range");

	    if (range_file.get_int() > 1)
		ret.push_back("/dev/" + name);
	}

	return ret;
    }


    void
    Disk::Impl::probe(SystemInfo& systeminfo)
    {
	BlkDevice::Impl::probe(systeminfo);

	const File rotational_file = systeminfo.getFile(SYSFSDIR + get_sysfs_path() + "/queue/rotational");
	rotational = rotational_file.get_int() != 0;

	Lsscsi::Entry entry;
	if (systeminfo.getLsscsi().getEntry(get_name(), entry))
	    transport = entry.transport;

	const Parted& parted = systeminfo.getParted(get_name());
	if (parted.getLabel() == PtType::MSDOS || parted.getLabel() == PtType::GPT)
	{
	    PartitionTable* pt = create_partition_table(parted.getLabel());
	    pt->get_impl().probe(systeminfo);
	}

	geometry = parted.getGeometry();
    }


    void
    Disk::Impl::save(xmlNode* node) const
    {
	BlkDevice::Impl::save(node);

	setChildValue(node, "geometry", geometry);

	setChildValueIf(node, "rotational", rotational, rotational);

	setChildValueIf(node, "transport", toString(transport), transport != TUNKNOWN);
    }


    PartitionTable*
    Disk::Impl::create_partition_table(PtType pt_type)
    {
	if (get_device()->num_children() != 0)
	    throw runtime_error("has children");

	PartitionTable* ret = nullptr;

	switch (pt_type)
	{
	    case PtType::MSDOS:
		ret = Msdos::create(get_devicegraph());
		break;

	    case PtType::GPT:
		ret = Gpt::create(get_devicegraph());
		break;

	    default:
		throw;
	}

	Using::create(get_devicegraph(), get_device(), ret);
	return ret;
    }


    PartitionTable*
    Disk::Impl::get_partition_table()
    {
	if (get_device()->num_children() != 1)
	    throw runtime_error("has no children");

	const Devicegraph* devicegraph = get_devicegraph();

	Device* child = devicegraph->get_impl().graph[devicegraph->get_impl().child(get_vertex())].get();

	return dynamic_cast<PartitionTable*>(child);
    }


    const PartitionTable*
    Disk::Impl::get_partition_table() const
    {
	if (get_device()->num_children() != 1)
	    throw runtime_error("has no children");

	const Devicegraph* devicegraph = get_devicegraph();

	const Device* child = devicegraph->get_impl().graph[devicegraph->get_impl().child(get_vertex())].get();

	return dynamic_cast<const PartitionTable*>(child);
    }


    void
    Disk::Impl::add_create_actions(Actiongraph& actiongraph) const
    {
	const Environment& environment = actiongraph.get_storage().get_environment();
	if (environment.get_target_mode() == TargetMode::TARGET_IMAGE)
	{
	    vector<Action::Base*> actions;
	    actions.push_back(new Action::Create(get_sid()));
	    actiongraph.add_chain(actions);
	}
	else
	{
	    throw runtime_error("cannot create disk");
	}
    }


    void
    Disk::Impl::add_delete_actions(Actiongraph& actiongraph) const
    {
	throw runtime_error("cannot delete disk");
    }


    bool
    Disk::Impl::equal(const Device::Impl& rhs_base) const
    {
	const Impl& rhs = dynamic_cast<const Impl&>(rhs_base);

	if (!BlkDevice::Impl::equal(rhs))
	    return false;

	return geometry == rhs.geometry && rotational == rhs.rotational &&
	    transport == rhs.transport;
    }


    void
    Disk::Impl::log_diff(std::ostream& log, const Device::Impl& rhs_base) const
    {
	const Impl& rhs = dynamic_cast<const Impl&>(rhs_base);

	BlkDevice::Impl::log_diff(log, rhs);

	storage::log_diff(log, "geometry", geometry, rhs.geometry);

	storage::log_diff(log, "rotational", rotational, rhs.rotational);

	storage::log_diff_enum(log, "transport", transport, rhs.transport);
    }


    void
    Disk::Impl::print(std::ostream& out) const
    {
	BlkDevice::Impl::print(out);

	out << " geometry:" << geometry;

	if (rotational)
	    out << " rotational";

	out << " transport:" << toString(get_transport());
    }


    void
    Disk::Impl::process_udev_ids(vector<string>& udev_ids) const
    {
	udev_ids.erase(remove_if(udev_ids.begin(), udev_ids.end(), string_starts_with("edd-")),
		       udev_ids.end());

	partition(udev_ids.begin(), udev_ids.end(), string_starts_with("ata-"));
    }


    Text
    Disk::Impl::do_create_text(bool doing) const
    {
	return sformat(_("Create hard disk %1$s (%2$s)"), get_displayname().c_str(),
		       get_size_string().c_str());
    }

}
