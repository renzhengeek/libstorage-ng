
#include <iostream>

#include <storage/SystemInfo/SystemInfo.h>

using namespace std;
using namespace storage;


void
test_parted(SystemInfo& system_info, const string& device)
{
    try
    {
	const Parted& parted = system_info.getParted(device);
	cout << "Parted success for '" << device << "'" << endl;
	cout << parted << endl;
    }
    catch (const exception& e)
    {
	cerr << "Parted failed for '" << device << "'" << endl;
    }
}


int
main()
{
    set_logger(get_logfile_logger());

    SystemInfo system_info;

    test_parted(system_info, "/dev/sda");
}
