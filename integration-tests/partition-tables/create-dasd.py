#!/usr/bin/python

# requirements: dasd /dev/dasdb

# Since parted reports a DASD parition table even if there is none this test
# first removed the partition table (which does not work on disk since wipefs
# does not know about DASD partition tables) and then creates a new. As a
# result the "volume serial" as displayed by fdasd is reset.


from storage import *
from storageitu import *


set_logger(get_logfile_logger())

environment = Environment(False)

storage = Storage(environment)
storage.probe()

staging = storage.get_staging()

print staging

dasd = Dasd.find_by_name(staging, "/dev/dasdb")

dasd.remove_descendants()

dasd.create_partition_table(PtType_DASD)

print staging

commit(storage)

