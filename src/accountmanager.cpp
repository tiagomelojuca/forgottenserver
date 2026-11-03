// Copyright 2022 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "accountmanager.h"

#include "configmanager.h"
#include "database.h"
#include "game.h"
#include "iologindata.h"
#include "monsters.h"
#include "vocation.h"

#include <fmt/format.h>

extern ConfigManager g_config;
extern Game g_game;
extern Monsters g_monsters;
extern Vocations g_vocations;

const std::string AccountManager::NAME = "Account Manager";

namespace {

enum class Step : uint8_t {
	NONE,

	ACCOUNT_NAME,
	ACCOUNT_NAME_CONFIRM,
	ACCOUNT_PASSWORD,
	ACCOUNT_PASSWORD_CONFIRM,

	CHARACTER_NAME,
	CHARACTER_NAME_CONFIRM,
	CHARACTER_SEX,
	CHARACTER_VOCATION,
	CHARACTER_VOCATION_CONFIRM,

	PASSWORD,
	PASSWORD_CONFIRM,
};

struct Session {
	uint32_t accountId = 0;
	Step step = Step::NONE;

	std::string name;
	std::string password;
	PlayerSex_t sex = PLAYERSEX_MALE;
	uint16_t vocationId = VOCATION_NONE;
};

std::unordered_map<uint32_t, Session> sessions;

constexpr size_t MAX_CHARACTERS_PER_ACCOUNT = 20;

void say(const Player* player, const std::string& message)
{
	player->sendTextMessage(MESSAGE_STATUS_CONSOLE_BLUE, message);
}

void greet(const Player* player, const Session& session)
{
	if (session.accountId == 0) {
		say(player, "Hello, type {account} to create a new account. You can type {cancel} at any moment to start over.");
	} else {
		say(player, "Hello, type {character} to create a new character or {password} to change your password. You can type {cancel} at any moment to start over.");
	}
}

bool isYes(const std::string& text)
{
	return text == "yes" || text == "y";
}

bool isNo(const std::string& text)
{
	return text == "no" || text == "n";
}

Town* getNewPlayerTown()
{
	Town* town = g_game.map.towns.getTown(static_cast<uint32_t>(g_config.getNumber(ConfigManager::NEW_PLAYER_TOWN_ID)));
	if (town) {
		return town;
	}

	const auto& towns = g_game.map.towns.getTowns();
	return towns.empty() ? nullptr : towns.begin()->second;
}

bool accountExists(const std::string& name)
{
	Database& db = Database::getInstance();
	return db.storeQuery(fmt::format("SELECT `id` FROM `accounts` WHERE `name` = {:s}", db.escapeString(name))) != nullptr;
}

size_t getCharacterCount(uint32_t accountId)
{
	DBResult_ptr result = Database::getInstance().storeQuery(fmt::format("SELECT COUNT(*) AS `count` FROM `players` WHERE `account_id` = {:d}", accountId));
	return result ? result->getNumber<size_t>("count") : 0;
}

// returns an empty string if the account name is valid, otherwise the reason why it is not
std::string validateAccountName(const std::string& name)
{
	if (name.size() < 3 || name.size() > 32) {
		return "Your account name must be between 3 and 32 characters long.";
	}

	if (!std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)); })) {
		return "Your account name may only contain letters and numbers.";
	}

	if (accountExists(name)) {
		return "An account with this name already exists, please choose another one.";
	}
	return {};
}

std::string validatePassword(const std::string& password)
{
	if (password.size() < 6 || password.size() > 29) {
		return "Your password must be between 6 and 29 characters long.";
	}

	if (!std::all_of(password.begin(), password.end(), [](char c) { return c > 32 && c < 127; })) {
		return "Your password contains invalid characters.";
	}
	return {};
}

// capitalizes the first letter of each word: "jOHN doe" -> "John Doe"
std::string formatCharacterName(std::string name)
{
	bool upper = true;
	for (char& c : name) {
		if (c == ' ') {
			upper = true;
		} else if (upper) {
			c = std::toupper(static_cast<unsigned char>(c));
			upper = false;
		} else {
			c = std::tolower(static_cast<unsigned char>(c));
		}
	}
	return name;
}

std::string validateCharacterName(const std::string& name)
{
	if (name.size() < 3 || name.size() > 25) {
		return "Your character name must be between 3 and 25 characters long.";
	}

	if (!std::all_of(name.begin(), name.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == ' '; })) {
		return "Your character name may only contain letters and spaces.";
	}

	if (name.front() == ' ' || name.back() == ' ' || name.find("  ") != std::string::npos) {
		return "Your character name contains invalid spaces.";
	}

	const std::string lowerName = asLowerCaseString(name);
	for (const char* prefix : {"gm ", "cm ", "god ", "tutor ", "admin", "account manager"}) {
		if (lowerName.compare(0, std::strlen(prefix), prefix) == 0) {
			return "Your character name is not allowed.";
		}
	}

	if (g_monsters.getMonsterType(name)) {
		return "Your character name can not be the name of a creature.";
	}

	if (IOLoginData::getGuidByName(name) != 0) {
		return "A character with this name already exists, please choose another one.";
	}
	return {};
}

std::vector<const Vocation*> getStartVocations()
{
	std::vector<const Vocation*> vocations;
	for (const auto& it : g_vocations.getVocations()) {
		const Vocation& vocation = it.second;
		if (vocation.getId() != VOCATION_NONE && vocation.getFromVocation() == vocation.getId()) {
			vocations.push_back(&vocation);
		}
	}
	return vocations;
}

std::string getVocationOptions()
{
	std::string options;
	const auto vocations = getStartVocations();
	for (size_t i = 0; i < vocations.size(); ++i) {
		if (i != 0) {
			options += (i + 1 == vocations.size()) ? " or " : ", ";
		}
		options += fmt::format("{{{:s}}}", asLowerCaseString(vocations[i]->getVocName()));
	}
	return options;
}

bool createAccount(Session& session)
{
	if (accountExists(session.name)) {
		return false;
	}

	Database& db = Database::getInstance();
	if (!db.executeQuery(fmt::format("INSERT INTO `accounts` (`name`, `password`, `creation`) VALUES ({:s}, {:s}, {:d})", db.escapeString(session.name), db.escapeString(transformToSHA1(session.password)), time(nullptr)))) {
		return false;
	}

	session.accountId = static_cast<uint32_t>(db.getLastInsertId());
	return session.accountId != 0;
}

bool createCharacter(const Session& session)
{
	Town* town = getNewPlayerTown();
	Vocation* vocation = g_vocations.getVocation(session.vocationId);
	Vocation* noVocation = g_vocations.getVocation(VOCATION_NONE);
	if (!town || !vocation || !noVocation) {
		return false;
	}

	uint32_t level = std::max<int32_t>(1, g_config.getNumber(ConfigManager::NEW_PLAYER_LEVEL));
	uint32_t magLevel = std::max<int32_t>(0, g_config.getNumber(ConfigManager::NEW_PLAYER_MAGIC_LEVEL));

	// level 1 base stats, then the same gains a player would have by leveling up
	// (vocation "None" up to level 8, as in rookgaard, and its own vocation after that)
	uint32_t health = 150, mana = 55, capacity = 400;
	for (uint32_t lv = 1; lv < level; ++lv) {
		const Vocation* gainVocation = lv < 8 ? noVocation : vocation;
		health += gainVocation->getHPGain();
		mana += gainVocation->getManaGain();
		capacity += gainVocation->getCapGain() / 100;
	}

	uint16_t lookType = session.sex == PLAYERSEX_MALE ? 128 : 136;

	Database& db = Database::getInstance();
	return db.executeQuery(fmt::format("INSERT INTO `players` (`name`, `account_id`, `group_id`, `level`, `vocation`, `health`, `healthmax`, `experience`, `lookbody`, `lookfeet`, `lookhead`, `looklegs`, `looktype`, `maglevel`, `mana`, `manamax`, `town_id`, `conditions`, `cap`, `sex`) VALUES ({:s}, {:d}, 1, {:d}, {:d}, {:d}, {:d}, {:d}, 68, 76, 78, 58, {:d}, {:d}, {:d}, {:d}, {:d}, '', {:d}, {:d})",
		db.escapeString(session.name), session.accountId, level, session.vocationId, health, health, Player::getExpForLevel(level), lookType, magLevel, mana, mana, town->getID(), capacity, static_cast<uint16_t>(session.sex)));
}

void finishCharacter(const Player* player, Session& session)
{
	if (createCharacter(session)) {
		say(player, fmt::format("Your character {:s} has been created. Log out and log in again to play with it. Would you like to create another {{character}}?", session.name));
	} else {
		say(player, "Your character could not be created, please try again later.");
	}
	session.step = Step::NONE;
}

}

bool AccountManager::isEnabled()
{
	return g_config.getBoolean(ConfigManager::ACCOUNT_MANAGER);
}

bool AccountManager::isLoginAccount(const std::string& accountName, const std::string& password)
{
	return isEnabled() && accountName == "1" && password == "1";
}

bool AccountManager::load(Player* player, uint32_t accountId)
{
	Group* group = g_game.groups.getGroup(1);
	Town* town = getNewPlayerTown();
	if (!group || !town || !player->setVocation(VOCATION_NONE)) {
		return false;
	}

	player->accountManager = true;
	player->accountNumber = accountId;
	player->setGroup(group);
	player->town = town;
	player->loginPosition = town->getTemplePosition();

	player->level = 1;
	player->experience = 0;
	player->health = player->healthMax = 150;
	player->mana = player->manaMax = 0;
	player->setSex(PLAYERSEX_MALE);
	player->defaultOutfit.lookType = 128;
	player->currentOutfit = player->defaultOutfit;

	// it only exists to talk to: no one else can see it and it can not walk
	player->ghostMode = true;
	player->setMovementBlocked(true);

	sessions[player->getID()].accountId = accountId;
	return true;
}

void AccountManager::onLogin(Player* player)
{
	auto it = sessions.find(player->getID());
	if (it != sessions.end()) {
		greet(player, it->second);
	}
}

void AccountManager::onLogout(Player* player)
{
	sessions.erase(player->getID());
}

void AccountManager::onSay(Player* player, const std::string& text)
{
	auto it = sessions.find(player->getID());
	if (it == sessions.end()) {
		return;
	}

	Session& session = it->second;

	std::string input = text;
	trimString(input);
	const std::string lowerInput = asLowerCaseString(input);

	if (lowerInput == "cancel") {
		session.step = Step::NONE;
		greet(player, session);
		return;
	}

	switch (session.step) {
		case Step::NONE: {
			if (session.accountId == 0 && lowerInput == "account") {
				session.step = Step::ACCOUNT_NAME;
				say(player, "To create an account you must choose an account name. What would you like your account name to be?");
			} else if (session.accountId != 0 && lowerInput == "character") {
				if (getCharacterCount(session.accountId) >= MAX_CHARACTERS_PER_ACCOUNT) {
					say(player, "Your account has reached the limit of characters.");
					break;
				}
				session.step = Step::CHARACTER_NAME;
				say(player, "What would you like as your character name?");
			} else if (session.accountId != 0 && lowerInput == "password") {
				session.step = Step::PASSWORD;
				say(player, "Tell me your new password please.");
			} else {
				greet(player, session);
			}
			break;
		}

		case Step::ACCOUNT_NAME: {
			std::string error = validateAccountName(input);
			if (!error.empty()) {
				say(player, error);
				break;
			}

			session.name = input;
			session.step = Step::ACCOUNT_NAME_CONFIRM;
			say(player, fmt::format("{:s}, are you sure? {{yes}} or {{no}}?", input));
			break;
		}

		case Step::ACCOUNT_NAME_CONFIRM: {
			if (isYes(lowerInput)) {
				session.step = Step::ACCOUNT_PASSWORD;
				say(player, "What would you like your password to be?");
			} else if (isNo(lowerInput)) {
				session.step = Step::ACCOUNT_NAME;
				say(player, "What else would you like as your account name?");
			} else {
				say(player, "Please answer {yes} or {no}.");
			}
			break;
		}

		case Step::ACCOUNT_PASSWORD: {
			std::string error = validatePassword(input);
			if (!error.empty()) {
				say(player, error);
				break;
			}

			session.password = input;
			session.step = Step::ACCOUNT_PASSWORD_CONFIRM;
			say(player, fmt::format("{:s} is it? {{yes}} or {{no}}?", input));
			break;
		}

		case Step::ACCOUNT_PASSWORD_CONFIRM: {
			if (isYes(lowerInput)) {
				if (createAccount(session)) {
					session.step = Step::NONE;
					say(player, fmt::format("Your account has been created! You can now log in with the account name {:s}. Would you like to create a {{character}} for it now?", session.name));
				} else {
					session.step = Step::ACCOUNT_NAME;
					say(player, "Your account could not be created, the account name may have been taken. What would you like your account name to be?");
				}
				session.password.clear();
			} else if (isNo(lowerInput)) {
				session.step = Step::ACCOUNT_PASSWORD;
				say(player, "What else would you like as your password?");
			} else {
				say(player, "Please answer {yes} or {no}.");
			}
			break;
		}

		case Step::CHARACTER_NAME: {
			std::string name = formatCharacterName(input);
			std::string error = validateCharacterName(name);
			if (!error.empty()) {
				say(player, error);
				break;
			}

			session.name = name;
			session.step = Step::CHARACTER_NAME_CONFIRM;
			say(player, fmt::format("{:s}, are you sure? {{yes}} or {{no}}?", name));
			break;
		}

		case Step::CHARACTER_NAME_CONFIRM: {
			if (isYes(lowerInput)) {
				session.step = Step::CHARACTER_SEX;
				say(player, "Should your character be a {male} or a {female}?");
			} else if (isNo(lowerInput)) {
				session.step = Step::CHARACTER_NAME;
				say(player, "What else would you like to name your character?");
			} else {
				say(player, "Please answer {yes} or {no}.");
			}
			break;
		}

		case Step::CHARACTER_SEX: {
			if (lowerInput == "male") {
				session.sex = PLAYERSEX_MALE;
			} else if (lowerInput == "female") {
				session.sex = PLAYERSEX_FEMALE;
			} else {
				say(player, "Please answer {male} or {female}.");
				break;
			}

			session.vocationId = VOCATION_NONE;
			if (g_config.getBoolean(ConfigManager::NEW_PLAYER_CHOOSE_VOC) && !getStartVocations().empty()) {
				session.step = Step::CHARACTER_VOCATION;
				say(player, fmt::format("What would you like to be... {:s}?", getVocationOptions()));
			} else {
				finishCharacter(player, session);
			}
			break;
		}

		case Step::CHARACTER_VOCATION: {
			int32_t vocationId = g_vocations.getVocationId(lowerInput);
			const auto vocations = getStartVocations();
			auto it = std::find_if(vocations.begin(), vocations.end(), [vocationId](const Vocation* vocation) { return vocation->getId() == vocationId; });
			if (it == vocations.end()) {
				say(player, fmt::format("I don't understand what vocation you would like to be. Please choose {:s}.", getVocationOptions()));
				break;
			}

			session.vocationId = (*it)->getId();
			session.step = Step::CHARACTER_VOCATION_CONFIRM;
			say(player, fmt::format("So you would like to be {:s}... {{yes}} or {{no}}?", (*it)->getVocDescription()));
			break;
		}

		case Step::CHARACTER_VOCATION_CONFIRM: {
			if (isYes(lowerInput)) {
				finishCharacter(player, session);
			} else if (isNo(lowerInput)) {
				session.step = Step::CHARACTER_VOCATION;
				say(player, fmt::format("What would you like to be then... {:s}?", getVocationOptions()));
			} else {
				say(player, "Please answer {yes} or {no}.");
			}
			break;
		}

		case Step::PASSWORD: {
			std::string error = validatePassword(input);
			if (!error.empty()) {
				say(player, error);
				break;
			}

			session.password = input;
			session.step = Step::PASSWORD_CONFIRM;
			say(player, fmt::format("Should {:s} be your new password? {{yes}} or {{no}}?", input));
			break;
		}

		case Step::PASSWORD_CONFIRM: {
			if (isYes(lowerInput)) {
				Database& db = Database::getInstance();
				if (db.executeQuery(fmt::format("UPDATE `accounts` SET `password` = {:s} WHERE `id` = {:d}", db.escapeString(transformToSHA1(session.password)), session.accountId))) {
					say(player, "Your password has been changed.");
				} else {
					say(player, "Your password could not be changed, please try again later.");
				}
				session.password.clear();
				session.step = Step::NONE;
			} else if (isNo(lowerInput)) {
				session.step = Step::PASSWORD;
				say(player, "Then tell me your new password please.");
			} else {
				say(player, "Please answer {yes} or {no}.");
			}
			break;
		}
	}
}
