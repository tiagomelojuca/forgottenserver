// Copyright 2022 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_ACCOUNTMANAGER_H_6F1C2A5E8B3D4F0A9E7C1B2D3A4F5E6C
#define FS_ACCOUNTMANAGER_H_6F1C2A5E8B3D4F0A9E7C1B2D3A4F5E6C

class Player;

class AccountManager
{
	public:
		static const std::string NAME;

		static bool isEnabled();
		// account "1" with password "1" logs in to create a new account
		static bool isLoginAccount(const std::string& accountName, const std::string& password);

		// accountId 0 means a new account is going to be created
		static bool load(Player* player, uint32_t accountId);
		static void onLogin(Player* player);
		static void onLogout(Player* player);
		static void onSay(Player* player, const std::string& text);
};

#endif
