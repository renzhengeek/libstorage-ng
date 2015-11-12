/*
 * Copyright (c) [2004-2014] Novell, Inc.
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


#ifndef STORAGE_CMD_CRYPTSETUP_H
#define STORAGE_CMD_CRYPTSETUP_H


#include <string>
#include <vector>

#include "storage/StorageInterface.h"


namespace storage
{
    using std::string;
    using std::vector;


    using namespace storage_legacy;


    class CmdCryptsetup
    {

    public:

	CmdCryptsetup(const string& name);

	friend std::ostream& operator<<(std::ostream& s, const CmdCryptsetup& cmdcryptsetup);

	EncryptType encrypt_type;

    private:

	void parse(const vector<string>& lines);

	string name;

    };

}

#endif
