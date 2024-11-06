#pragma once

#include "../../../Core/Cache.h"
#include "../../ACommand.h"
#include "parsec-dso.h"

using namespace std;

class CommandMod : public ACommand
{
public:

	std::string usage = "Usage: !mod <username>\nExample: !mod RefffiK\0";

	/**
	 * @brief Construct a new CommandBan object
	 *
	 * @param msg
	 * @param sender
	 * @param parsec
	 * @param guests
	 * @param guestHistory
	 */
	CommandMod(const char* msg, Guest& sender, GuestList& guests, GuestDataList& guestHistory)
		: ACommand(msg, sender), guests(guests), _guestHistory(guestHistory)
	{
	}

	/**
	 * @brief Run the command
	 *
	 * @return true
	 * @return false
	 */
	bool run() override {

		// Was a guest specified?
		if (getArgs().size() == 0) {
			setReply(usage);
			return false;
		}

		// Find the guest
		if (findGuest()) {
			GuestData targetData(target.name, target.userID);
			return handleGuest(targetData, true, target.id);
		}

		// Find offline guest
		string guest = getArgs().size() > 0 ? getArgs()[0] : "";
		if (guest == "") {
			return false;
		}
		bool found = false;
		try {
			found = _guestHistory.find(guest, [&](GuestData& guest) { _offlineGuest = guest; });
		}
		catch (const std::exception&) {}
		if (found) {
			return handleGuest(_offlineGuest, false);
		}
		return false;

	}

	/**
	 * @brief Get the prefixes object
	 *
	 * @return vector<const char*>
	 */
	static vector<const char*> prefixes() {
		return vector<const char*> { "!ban", "!block" };
	}

private:
	static vector<const char*> internalPrefixes() {
		return vector<const char*> { "!ban ", "!block " };
	}

	ParsecDSO* _parsec;
	Guest target;
	GuestList guests;
	GuestDataList& _guestHistory;
	GuestData _offlineGuest;

	/**
	* Get the guest referenced in the command. Returns nullptr
	* if no guest is found
	*
	* @param guestList The guest list
	*/
	bool findGuest() {

		// Get the guest
		string guest = getArgs().size() > 0 ? getArgs()[0] : "";
		if (guest == "") {
			return false;
		}

		try {
			uint32_t id = stoul(guest);
			vector<Guest>::iterator i;
			for (i = guests.getGuests().begin(); i != guests.getGuests().end(); ++i) {
				if ((*i).userID == id) {
					target = *i;
					return true;
				}
			}
		}
		catch (const std::exception&) {
			bool found = guests.find(guest, &target);
			if (found) {
				return true;
			}
		}

		return false;
	}

	/**
	 * @brief Handle the guest
	 *
	 * @param target
	 * @param isOnline
	 * @param guestID
	 * @return true
	 * @return false
	 */
	bool handleGuest(GuestData target, bool isOnline, uint32_t guestID = -1) {
		if (!Cache::cache.modList.isModded(target.userID)) {
			if (Cache::cache.modList.mod(target)) {
				setReply("Made " + target.name + " a mod!\0");
				return true;
			}
		}
		else {
			setReply(target.name + " is already a mod!\0");
			return false;
		}
	}
};