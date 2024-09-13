#include "Hosting.h"

using namespace std;

#if defined(_WIN32)
	#if !defined(BITS)
		#define BITS 64
	#endif
	#if (BITS == 64)
		#define SDK_PATH "./parsec.dll"
	#else
		#define SDK_PATH "./parsec32.dll"
	#endif
#endif

// ============================================================
// 
//  PUBLIC
// 
// ============================================================

Hosting::Hosting()
{

	// Create a random 8 character string
	_roomToken = "";
	
	_hostConfig = EMPTY_HOST_CONFIG;
	MetadataCache::loadPreferences();
	setHostConfig(
		"",
		"",
		Config::cfg.room.guestLimit,
		false,
		Config::cfg.room.secret
	);
	setHostVideoConfig(Config::cfg.video.fps, Config::cfg.video.bandwidth);
	
	_tierList.loadTiers();
	_tierList.saveTiers();

	_parsec = nullptr;

	SDL_Init(SDL_INIT_JOYSTICK);
	_masterOfPuppets.init(_gamepadClient);
	_masterOfPuppets.start();

	_latencyLimitEnabled = Config::cfg.room.latencyLimit;
	_latencyLimitThreshold = Config::cfg.room.latencyLimitThreshold;
	_disableGuideButton = Config::cfg.input.disableGuideButton;
	_disableKeyboard = Config::cfg.input.disableKeyboard;
	_lockedGamepad.bLeftTrigger = Config::cfg.input.lockedGamepadLeftTrigger;
	_lockedGamepad.bRightTrigger = Config::cfg.input.lockedGamepadRightTrigger;
	_lockedGamepad.sThumbLX = Config::cfg.input.lockedGamepadLX;
	_lockedGamepad.sThumbLY = Config::cfg.input.lockedGamepadLY;
	_lockedGamepad.sThumbRX = Config::cfg.input.lockedGamepadRX;
	_lockedGamepad.sThumbRY = Config::cfg.input.lockedGamepadRY;
	_lockedGamepad.wButtons = Config::cfg.input.lockedGamepadButtons;
}

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
	((std::string*)userp)->append((char*)contents, size * nmemb);
	return size * nmemb;
}

/**
 * Handles logs generated by the Parsec SDK.
 * @param level The log level.
 * @param msg The log message.
 * @param opaque The opaque data.
 */
void Hosting::LogCallback(ParsecLogLevel level, const char *msg, void *opaque) {
	Hosting* self = static_cast<Hosting*>(opaque);
	if (Config::cfg.general.parsecLogs) {
		self->logMessage(msg);
	}

	// Try to find the IP address in the log message
	std::string message(msg);
    std::size_t start = message.find("BUD|");
    if (start != std::string::npos) {
        start += 4; // Skip "BUD|"
        std::size_t end = message.find("|", start);
        if (end != std::string::npos) {
            std::string ipAddressWithPrefix = message.substr(start, end - start);
            std::size_t ipStart = ipAddressWithPrefix.find("::ffff:");
            if (ipStart != std::string::npos) {
                ipStart += 7; // Skip "::ffff:"
                std::string ipAddress = ipAddressWithPrefix.substr(ipStart);
                Cache::cache.pendingIpAddress = ipAddress;
            }
        }
    }
}

void Hosting::applyHostConfig() {
	
	if (isRunning()){
		ParsecHostSetConfig(_parsec, &_hostConfig, _parsecSession.sessionId.c_str());
	}
}

void Hosting::broadcastChatMessage(string message)
{
	vector<Guest> guests = _guestList.getGuests();
	vector<Guest>::iterator gi;

	for (gi = guests.begin(); gi != guests.end(); ++gi)
	{
		ParsecHostSendUserData(_parsec, (*gi).id, HOSTING_CHAT_MSG_ID, message.c_str());
	}
}

void Hosting::init() {
	
	_parsecStatus = ParsecInit(NULL, NULL, (char *)SDK_PATH, &_parsec);
	_dx11.init();
	_gamepadClient.setParsec(_parsec);
	_gamepadClient.init();

	MetadataCache::Preferences preferences = MetadataCache::loadPreferences();

	_createGamepadsThread = thread([&]() {
		_gamepadClient.createAllGamepads();
		_createGamepadsThread.detach();
		_macro.init(_gamepadClient, _guestList);
		_tournament.init(*_parsec, _guestList, _gamepadClient, _chatLog, _macro);
	});

	audioOut.fetchDevices();
	vector<AudioOutDevice> audioOutDevices = audioOut.getDevices();
	if (Config::cfg.audio.outputDevice >= audioOutDevices.size()) {
		Config::cfg.audio.outputDevice = 0;
	}
	audioOut.setOutputDevice(Config::cfg.audio.outputDevice);
	audioOut.captureAudio();
	audioOut.volume = Config::cfg.audio.speakersVolume;
	audioOut.setFrequency((Frequency)Config::cfg.audio.speakersFrequency);

	vector<AudioInDevice> audioInputDevices = audioIn.listInputDevices();
	if (Config::cfg.audio.inputDevice >= audioInputDevices.size()) {
		Config::cfg.audio.inputDevice = 0;
	}
	AudioInDevice device = audioIn.selectInputDevice(Config::cfg.audio.inputDevice);
	audioIn.init(device);
	audioIn.volume = Config::cfg.audio.micVolume;
	audioIn.setFrequency((Frequency)Config::cfg.audio.micFrequency);

	preferences.isValid = true;
	MetadataCache::savePreferences(preferences);
	_parsecSession.loadSessionCache();

	fetchAccountData();

	_chatBot = new ChatBot(
		audioIn, audioOut, _dx11,
		_gamepadClient, _guestList, _guestHistory, _parsec,
		_hostConfig, _parsecSession,
		_macro, _isRunning, _host, _hotseat, _tournament
	);

	CommandBonk::init();

	// Parsec Logs
	ParsecSetLogCallback(_parsec, Hosting::LogCallback, this);

	// A way to test chat and guest managment features
	// without hosting the room
	_isTestMode = false;
	if (_isTestMode) {

		// Add (n) number of fake guests
		//srand(time(NULL));
		//addFakeGuests(8);
		
	}
	
}

void Hosting::release() {
	stopHosting();
	while (_isRunning)
	{
		Sleep(5);
	}
	_dx11.clear();
	_gamepadClient.release();
	_masterOfPuppets.stop();
	if (WebSocket::instance.isRunning()) {
		WebSocket::instance.stopServer();
	}
}

bool Hosting::isReady() {
	
	return _parsecStatus == PARSEC_OK;
}

bool Hosting::isRunning() {
	return _isTestMode || _isRunning;
}

bool& Hosting::isGamepadLock()
{
	return _gamepadClient.lock;
}

bool& Hosting::isGamepadLockButtons()
{
	return _gamepadClient.lockButtons;
}

Guest& Hosting::getHost()
{
	return _host;
}

ParsecSession& Hosting::getSession()
{
	return _parsecSession;
}

void Hosting::fetchAccountData(bool sync)
{
	_host.name = "Host";
	_host.status = Guest::Status::INVALID;
	if (isReady()) {
		if (sync) {
			_parsecSession.fetchAccountDataSync(&_host);
		}
		else {
			_parsecSession.fetchAccountData(&_host);
		}
	}
}

ParsecHostConfig& Hosting::getHostConfig()
{
	return _hostConfig;
}

DX11& Hosting::getDX11()
{
	return _dx11;
}

ChatBot* Hosting::getChatBot()
{
	return _chatBot;
}

vector<string>& Hosting::getMessageLog()
{
	return _chatLog.getMessageLog();
}

vector<string>& Hosting::getCommandLog()
{
	return _chatLog.getCommandLog();
}

GuestList& Hosting::getGuestList() {
	return _guestList;
}

vector<Guest>& Hosting::getGuests() {
	return _guestList.getGuests();
}

vector<Guest>& Hosting::getPlayingGuests() {
	return _guestList.getPlayingGuests();
}

vector<Guest>& Hosting::getRandomGuests() {
	return _guestList.getRandomGuests();
}

Guest& Hosting::getGuest(uint32_t id) {
	
	// For each guest in the guest list
	for (Guest& guest : _guestList.getGuests()) {

		// If the guest ID matches the target ID
		if (guest.id == id) {

			// Return the guest
			return guest;
		}
	}
	
}

int Hosting::getGuestIndex(uint32_t id) {

	// For each guest in the guest list
	for (int i = 0; i < _guestList.getGuests().size(); i++) {

		// If the guest ID matches the target ID
		if (_guestList.getGuests()[i].userID == id) {

			// Return the guest
			return i;
		}
	}

	return -1;

}

bool Hosting::guestExists(uint32_t id) {
	for (Guest& guest : _guestList.getGuests()) {
		if (guest.id == id) {
			return true;
		}
	}
	return false;
}

vector<Guest>& Hosting::getGuestsAfterGuest(uint32_t targetGuestID, int count, bool ignoreSpectators) {
	return _guestList.getGuestsAfterGuest(targetGuestID, count, ignoreSpectators);
}

vector<GuestData>& Hosting::getGuestHistory()
{
	return _guestHistory.getGuests();
}

MyMetrics Hosting::getMetrics(uint32_t id) {
	return _guestList.getMetrics(id);
}

vector<AGamepad*>& Hosting::getGamepads() {
	return _gamepadClient.gamepads;
}

GamepadClient& Hosting::getGamepadClient()
{
	return _gamepadClient;
}

MasterOfPuppets& Hosting::getMasterOfPuppets()
{
	return _masterOfPuppets;
}

const char** Hosting::getGuestNames()
{
	return _guestList.guestNames;
}

void Hosting::toggleGamepadLock()
{
	_gamepadClient.toggleLock();
}

void Hosting::toggleGamepadLockButtons()
{
	_gamepadClient.toggleLockButtons();
}

void Hosting::setGameID(string gameID)
{
	try
	{
		strcpy_s(_hostConfig.gameID, gameID.c_str());
	}
	catch (const std::exception&) {}
}

void Hosting::setMaxGuests(uint8_t maxGuests)
{
	_hostConfig.maxGuests = maxGuests;
}

void Hosting::setHostConfig(string roomName, string gameId, uint8_t maxGuests, bool isPublicRoom)
{
	setRoomName(roomName);
	setGameID(gameId);
	setMaxGuests(maxGuests);
	setPublicRoom(isPublicRoom);
}

void Hosting::setHostConfig(string roomName, string gameId, uint8_t maxGuests, bool isPublicRoom, string secret) {
	setRoomName(roomName);
	setGameID(gameId);
	setMaxGuests(maxGuests);
	setPublicRoom(isPublicRoom);
	setRoomSecret(secret);
}

void Hosting::setHostVideoConfig(uint32_t fps, uint32_t bandwidth) {
	if (Config::cfg.video.resolutionIndex > 0) {
		_hostConfig.video->resolutionX = Config::cfg.resolutions[Config::cfg.video.resolutionIndex].width;
		_hostConfig.video->resolutionY = Config::cfg.resolutions[Config::cfg.video.resolutionIndex].height;
	}
	_hostConfig.video->encoderFPS = fps;
	_hostConfig.video->encoderMaxBitrate = bandwidth;
	Config::cfg.video.fps = fps;
	Config::cfg.video.bandwidth = bandwidth;
}

void Hosting::setPublicRoom(bool isPublicRoom) {
	_hostConfig.publicGame = isPublicRoom;
}

void Hosting::setRoomName(string roomName)
{
	try
	{
		strcpy_s(_hostConfig.name, roomName.c_str());
	}
	catch (const std::exception&) {}
}

void Hosting::setRoomSecret(string secret)
{

	try
	{
		strcpy_s(_hostConfig.secret, secret.c_str());
	}
	catch (const std::exception&) {}
}

/// <summary>
/// Starts hosting the Parsec session.
/// </summary>
void Hosting::startHosting() {

	// Update config
	Config::cfg.Save();

	if (!_isRunning) {
		_isRunning = true;
		initAllModules();

		try
		{
			if (_parsec != nullptr)
			{
				_mediaThread = thread ( [this]() {liveStreamMedia(); } );
				_inputThread = thread ([this]() {pollInputs(); });
				_eventThread = thread ([this]() {pollEvents(); });
				_latencyThread = thread([this]() {pollLatency(); });
				_smashSodaThread = thread([this]() { pollSmashSoda(); });
				_mainLoopControlThread = thread ([this]() {mainLoopControl(); });

				// Start the chat automoderator
				AutoMod::instance.Start();

				// Hotseat mode
				if (Config::cfg.hotseat.enabled) {
					Hotseat::instance.Start();
				}

				// Overlay
				if (Config::cfg.overlay.enabled) {
				}

			}
		}
		catch (const exception&)
		{}
	}

	if (Config::cfg.socket.enabled) {
		WebSocket::instance.createServer(9002);
	}

	bool debug = false;
}

/// <summary>
/// Stops hosting the Parsec session.
/// </summary>
void Hosting::stopHosting() {

	// Stop the chat automoderator
	AutoMod::instance.Stop();

	// Remove post on Soda Arcade
	if (!Config::cfg.room.privateRoom) {
		Arcade::instance.deletePost();
	}

	_isRunning = false;
	_guestList.clear();

	// Stop hotseat mode
	if (Config::cfg.hotseat.enabled) {
		Hotseat::instance.Stop();
	}

	// Stop kiosk mode
	if (Config::cfg.kioskMode.enabled) {
		ProcessMan::instance.stop();
	}

	// Stop web socket server
	if (WebSocket::instance.isRunning()) {
		WebSocket::instance.stopServer();
	}

}

void Hosting::stripGamepad(int index)
{
	_gamepadClient.clearOwner(index);
}

void Hosting::setOwner(AGamepad& gamepad, Guest newOwner, int padId)
{
	bool found = _gamepadClient.findPreferences(newOwner.userID, [&](GamepadClient::GuestPreferences& prefs) {
		gamepad.setOwner(newOwner, padId, prefs.mirror);
	});

	if (!found) {
		gamepad.setOwner(newOwner, padId, false);
	}
}

void Hosting::handleMessage(const char* message, Guest& guest, bool isHost, bool isHidden, bool outside) {
	
	// Has the user been muted?
	if (AutoMod::instance.isMuted(guest.userID)) {
		return;
	}

	ACommand* command = _chatBot->identifyUserDataMessage(message, guest, isHost);
	command->run();

	// Non-blocked default message
	if (!isFilteredCommand(command)) {
		Tier tier = _tierList.getTier(guest.userID);

		CommandDefaultMessage defaultMessage(message, guest, _chatBot->getLastUserId(), tier, isHost);
		defaultMessage.run();
		_chatBot->setLastUserId(guest.userID);

		if (!defaultMessage.replyMessage().empty() && !isHidden) {
			broadcastChatMessage(defaultMessage.replyMessage());

			string adjustedMessage = defaultMessage.replyMessage();
			Stringer::replacePatternOnce(adjustedMessage, "%", "%%");
			_chatLog.logMessage(adjustedMessage);
			if (WebSocket::instance.isRunning()) {
				json j;
				j["event"] = "chat:message";
				j["data"]["user"] = {
					{"id", guest.userID},
					{"name", guest.name},
				};
				j["data"]["message"] = message;
				WebSocket::instance.sendMessageToAll(j.dump());
			}

			// Record last message
			AutoMod::instance.RecordMessage(guest.userID, guest.name, message);
		}
	}

	// Chatbot's command reply
	if (!command->replyMessage().empty() && command->isBotCommand) {
		broadcastChatMessage(command->replyMessage());
		_chatLog.logCommand(command->replyMessage());
		_chatBot->setLastUserId();
		if (WebSocket::instance.isRunning()) {
			json j;
			j["event"] = "chat:log";
			j["data"]["message"] = command->replyMessage();
			WebSocket::instance.sendMessageToAll(j.dump());
		}
	}

	delete command;

}

/// <summary>
/// This handles muted guests and auto mutes anybody spamming chat.
/// </summary>
/// <param name="message">The message sent.</param>
/// <param name="guest">The guest who sent the message.</param>
/// <returns>false if muted</returns>
bool Hosting::handleMuting(const char* message, Guest& guest) {

	

	return true;

}

/// <summary>
/// 
/// </summary>
/// <param name="message"></param>
/// <param name="isHidden"></param>
void Hosting::sendHostMessage(const char* message, bool isHidden) {
	static bool isAdmin = true;
	handleMessage(message, _host, true, isHidden);
}

// ============================================================
// 
//  PRIVATE
// 
// ============================================================
void Hosting::initAllModules()
{
	// Instance all gamepads at once
	_connectGamepadsThread = thread([&]() {
		_gamepadClient.sortGamepads();
		_connectGamepadsThread.detach();
	});

	roomStart();
}

void Hosting::submitSilence()
{
	ParsecHostSubmitAudio(_parsec, PCM_FORMAT_INT16, audioOut.getFrequencyHz(), nullptr, 0);
}

void Hosting::liveStreamMedia()
{
	_mediaMutex.lock();
	_isMediaThreadRunning = true;

	static uint32_t sleepTimeMs = 4;
	_mediaClock.setDuration(sleepTimeMs);
	_mediaClock.start();

	while (_isRunning)
	{
		_mediaClock.reset();

		_dx11.captureScreen(_parsec);

		if (Config::cfg.audio.micEnabled && audioIn.isEnabled && audioOut.isEnabled)
		{
			audioIn.captureAudio();
			audioOut.captureAudio();
			if (audioIn.isReady() && audioOut.isReady())
			{
				vector<int16_t> mixBuffer = _audioMix.mix(audioIn.popBuffer(), audioOut.popBuffer());
				ParsecHostSubmitAudio(_parsec, PCM_FORMAT_INT16, audioOut.getFrequencyHz(), mixBuffer.data(), (uint32_t)mixBuffer.size() / 2);
			}
			else submitSilence();
		}
		else if (audioOut.isEnabled)
		{
			audioOut.captureAudio();
			if (audioOut.isReady())
			{
				vector<int16_t> buffer = audioOut.popBuffer();
				ParsecHostSubmitAudio(_parsec, PCM_FORMAT_INT16, audioOut.getFrequencyHz(), buffer.data(), (uint32_t)buffer.size() / 2);
			}
			else submitSilence();
		}
		else if (Config::cfg.audio.micEnabled && audioIn.isEnabled)
		{
			audioIn.captureAudio();
			if (audioIn.isReady())
			{
				vector<int16_t> buffer = audioIn.popBuffer();
				ParsecHostSubmitAudio(_parsec, PCM_FORMAT_INT16, (uint32_t)audioIn.getFrequency(), buffer.data(), (uint32_t)buffer.size() / 2);
			}
			else submitSilence();
		}
		else submitSilence();

		sleepTimeMs = _mediaClock.getRemainingTime();
		if (sleepTimeMs > 0)
		{
			Sleep(sleepTimeMs);
		}
	}

	_isMediaThreadRunning = false;
	_mediaMutex.unlock();
	_mediaThread.detach();
}

void Hosting::mainLoopControl()
{
	do
	{
		Sleep(50);
	} while (!_isRunning);

	_isRunning = true;

	_mediaMutex.lock();
	_inputMutex.lock();
	_eventMutex.lock();

	ParsecHostStop(_parsec);
	_isRunning = false;

	_mediaMutex.unlock();
	_inputMutex.unlock();
	_eventMutex.unlock();

	_mainLoopControlThread.detach();
}

void Hosting::pollEvents()
{
	_eventMutex.lock();
	_isEventThreadRunning = true;

	string chatBotReply;

	ParsecGuest* guests = nullptr;
	int guestCount = 0;

	ParsecHostEvent event;

	while (_isRunning)
	{
		if (ParsecHostPollEvents(_parsec, 30, &event)) {
			
			ParsecGuest parsecGuest = event.guestStateChange.guest;
			ParsecGuestState state = parsecGuest.state;
			Guest guest = Guest(parsecGuest.name, parsecGuest.userID, parsecGuest.id);
			guestCount = ParsecHostGetGuests(_parsec, GUEST_CONNECTED, &guests);
			_guestList.setGuests(guests, guestCount);
			//logMessage("event: " + to_string(event.type) + " guest: " + guest.name + " state: " + to_string(state) + " status: " + to_string(event.guestStateChange.status));

			// Is room full?
			if (event.guestStateChange.status == 1 && 
			(Cache::cache.modList.isModded(guest.userID) || Cache::cache.vipList.isVIP(guest.userID)) &&
				guestCount >= _hostConfig.maxGuests) {
				logMessage("VIP user " + guest.name + " is trying to join, making room for them.");

				// Make extra spot
				_hostConfig.maxGuests = _hostConfig.maxGuests + 1;
				MetadataCache::preferences.extraSpots++;
				ParsecHostSetConfig(_parsec, &_hostConfig, _parsecSession.sessionId.c_str());

				// Send user a test message
				ParsecHostSendUserData(_parsec, guest.id, HOSTING_CHAT_MSG_ID, "Welcome VIP user! We've made room for you.");
				
			} else {

				// Was an extra spot made?
				if (MetadataCache::preferences.extraSpots > 0) {
					_hostConfig.maxGuests = _hostConfig.maxGuests - 1;
					MetadataCache::preferences.extraSpots--;
					ParsecHostSetConfig(_parsec, &_hostConfig, _parsecSession.sessionId.c_str());
				}

			}

			switch (event.type)
			{
			case HOST_EVENT_GUEST_STATE_CHANGE:
				onGuestStateChange(state, guest, event.guestStateChange.status);
				break;

			case HOST_EVENT_USER_DATA:
				char* msg = (char*)ParsecGetBuffer(_parsec, event.userData.key);

				if (event.userData.id == PARSEC_APP_CHAT_MSG)
				{
					handleMessage(msg, guest);
				}

				ParsecFree(_parsec, msg);
				break;
			}
		}
	}

	ParsecFree(_parsec, guests);
	_isEventThreadRunning = false;
	_eventMutex.unlock();
	_eventThread.detach();
}

void Hosting::pollLatency()
{
	_latencyMutex.lock();
	_isLatencyThreadRunning = true;
	int guestCount = 0;
	ParsecGuest* guests = nullptr;
	while (_isRunning)
	{
		Sleep(2000);
		guestCount = ParsecHostGetGuests(_parsec, GUEST_CONNECTED, &guests);
		if (guestCount > 0) {
			_guestList.updateMetrics(guests, guestCount);

			// Latency limiter
			if (Config::cfg.room.latencyLimit) {
				for (size_t mi = 0; mi < guestCount; mi++) {
					MyMetrics m = _guestList.getMetrics(guests[mi].id);

					if (m.averageNetworkLatencySize > 5 &&
						m.averageNetworkLatency > Config::cfg.room.latencyLimitThreshold) {
						ParsecHostKickGuest(_parsec, guests[mi].id);
					}
				}
			}

			if (WebSocket::instance.isRunning()) {
				json j;
				j["event"] = "guest:poll";

				json users = json::array();
				for (Guest guest : getGuests()) {
					users.push_back({
						{"id", to_string(guest.userID)},
						{"tier", _tierList.getTier(guest.userID)},
						{"name", guest.name},
						{"latency", getGuestList().getMetrics(guest.id).metrics.networkLatency }
					});
				}
				j["data"]["users"] = users;

				WebSocket::instance.sendMessageToAll(j.dump());
			}

		}
		
	}
	_isLatencyThreadRunning = false;
	_latencyMutex.unlock();
	_latencyThread.detach();
}

/// <summary>
/// This thread handles all the new Smash Soda features!
/// </summary>
void Hosting::pollSmashSoda() {
	_smashSodaMutex.lock();
	_isSmashSodaThreadRunning = true;
	while (_isRunning) {

		Sleep(100);

		// Handles welcome messages and other new guest stuff
		handleNewGuests();

		// Handles all the automatic button press stuff
		_macro.run();

		// Updated room settings?
		if (Config::cfg.roomChanged) {
			ParsecHostSetConfig(_parsec, &_hostConfig, _parsecSession.sessionId.c_str());
			Config::cfg.roomChanged = false;
		}

		// Poll inputs
		if (WebSocket::instance.isRunning()) {
			json j;
			j["event"] = "gamepad:poll";
			json pads = json::array();
			for (AGamepad* gamepad : getGamepads()) {
				if (gamepad->isConnected()) {

					json pad;

					// Owner
					if (gamepad->owner.guest.userID != 0) {
						json owner;
						owner["id"] = gamepad->owner.guest.userID;
						owner["name"] = gamepad->owner.guest.name;

						owner["hotseatTime"] = Hotseat::instance.getUserTimeRemaining(gamepad->owner.guest.userID);

						pad["owner"] = owner;
					}

					// Button array
					WORD wButtons = gamepad->getState().Gamepad.wButtons;
					json buttons = json::array();
					buttons.push_back((wButtons & XUSB_GAMEPAD_A) != 0);
					buttons.push_back((wButtons & XUSB_GAMEPAD_B) != 0);
					buttons.push_back((wButtons & XUSB_GAMEPAD_X) != 0);
					buttons.push_back((wButtons & XUSB_GAMEPAD_Y) != 0);
					buttons.push_back((wButtons & XUSB_GAMEPAD_LEFT_SHOULDER) != 0);
					buttons.push_back((wButtons & XUSB_GAMEPAD_RIGHT_SHOULDER) != 0);
					buttons.push_back(gamepad->getState().Gamepad.bLeftTrigger > 0);
					buttons.push_back(gamepad->getState().Gamepad.bRightTrigger > 0);
					buttons.push_back((wButtons & XUSB_GAMEPAD_BACK) != 0);
					buttons.push_back((wButtons & XUSB_GAMEPAD_START) != 0);
					buttons.push_back((wButtons & XUSB_GAMEPAD_LEFT_THUMB) != 0);
					buttons.push_back((wButtons & XUSB_GAMEPAD_RIGHT_THUMB) != 0);
					buttons.push_back((wButtons & XUSB_GAMEPAD_DPAD_UP) != 0);
					buttons.push_back((wButtons & XUSB_GAMEPAD_DPAD_DOWN) != 0);
					buttons.push_back((wButtons & XUSB_GAMEPAD_DPAD_LEFT) != 0);
					buttons.push_back((wButtons & XUSB_GAMEPAD_DPAD_RIGHT) != 0);

					// Stick values
					ImVec2 leftStick;
					ImVec2 rightStick;
					float lDistance = 0, rDistance = 0;

					leftStick = stickShortToFloat(gamepad->getState().Gamepad.sThumbLX, gamepad->getState().Gamepad.sThumbLY, lDistance);
					rightStick = stickShortToFloat(gamepad->getState().Gamepad.sThumbRX, gamepad->getState().Gamepad.sThumbRY, rDistance);

					json axes = json::array();
					axes.push_back(leftStick.x);
					axes.push_back(leftStick.y);
					axes.push_back(rightStick.x);
					axes.push_back(rightStick.y);

					// Create the pad object
					pad["index"] = gamepad->getIndex();
					pad["buttons"] = buttons;
					pad["axes"] = axes;

					// Add the pad to the array
					pads.push_back(pad);

				}
			}

			j["data"]["gamepads"] = pads;

			WebSocket::instance.sendMessageToAll(j.dump());
		}

	}
	
	_isSmashSodaThreadRunning = false;
	_smashSodaMutex.unlock();
	_smashSodaThread.detach();

}

/// <summary>
/// Checks to see if a guest is a spectator.
/// </summary>
/// <param name="index"></param>
bool Hosting::isSpectator(int index) {

	return MetadataCache::isSpectating(_guestList.getGuests()[index].userID);

}


void Hosting::addNewGuest(Guest guest) {

	NewGuest newGuest;
	newGuest.guest = guest;
	newGuest.timer.setDuration(newGuestList.size() * 2000);
	newGuest.timer.start();
	newGuestList.push_back(newGuest);

	try {
		PlaySound(TEXT("./SFX/new_guest.wav"), NULL, SND_FILENAME | SND_NODEFAULT | SND_ASYNC);
	}
	catch (const std::exception&) {}

}

void Hosting::handleNewGuests() {

	// ParsecHostSendUserData(_parsec, guest.id, HOSTING_CHAT_MSG_ID, test.c_str());

	if (newGuestList.size() > 0) {

		// Get next guest
		NewGuest newGuest = newGuestList.front();

		// Ready to process
		if (newGuestList.front().timer.isFinished()) {

			// Welcome message
			string msg = Config::cfg.chat.welcomeMessage;
			msg = regex_replace(msg, regex("_PLAYER_"), newGuest.guest.name);
			ParsecHostSendUserData(_parsec, newGuestList.front().guest.id, HOSTING_CHAT_MSG_ID, msg.c_str());

			// Remove from new guests
			newGuestList.erase(newGuestList.begin());

		}

	}

}

bool Hosting::isLatencyRunning()
{
	return _isLatencyThreadRunning;
}

bool Hosting::isGamepadRunning() {
	return _isGamepadThreadRunning;
}

void Hosting::pollInputs() {
	
	_inputMutex.lock();
	_isInputThreadRunning = true;

	ParsecGuest inputGuest;
	ParsecMessage inputGuestMsg;

	while (_isRunning)
	{
		if (ParsecHostPollInput(_parsec, 4, &inputGuest, &inputGuestMsg))
		{
			if (!_gamepadClient.lock)
			{
					_gamepadClient.sendMessage(inputGuest, inputGuestMsg);
			}

		}
	}

	_isInputThreadRunning = false;
	_inputMutex.unlock();
	_inputThread.detach();
}

void Hosting::updateButtonLock(LockedGamepadState lockedGamepad) {
	_lockedGamepad = lockedGamepad;
	Config::cfg.input.lockedGamepadLeftTrigger = _lockedGamepad.bLeftTrigger;
	Config::cfg.input.lockedGamepadRightTrigger = _lockedGamepad.bRightTrigger;
	Config::cfg.input.lockedGamepadLX = _lockedGamepad.sThumbLX;
	Config::cfg.input.lockedGamepadLY = _lockedGamepad.sThumbLY;
	Config::cfg.input.lockedGamepadRX = _lockedGamepad.sThumbRX;
	Config::cfg.input.lockedGamepadRY = _lockedGamepad.sThumbRY;
	Config::cfg.input.lockedGamepadButtons = _lockedGamepad.wButtons;
}

bool Hosting::roomStart() {
	
	if (isReady()) {
		ParsecStatus status = ParsecHostStart(_parsec, HOST_DESKTOP, &_hostConfig, _parsecSession.sessionId.c_str());
		Sleep(2000);
		ParsecHostStop(_parsec);
		Sleep(1000);
		status = ParsecHostStart(_parsec, HOST_GAME, &_hostConfig, _parsecSession.sessionId.c_str());

		// Init overlay
		if (Config::cfg.overlay.enabled) {
			
		}

		// Send room to Soda Arcade
		if (!Config::cfg.room.privateRoom) {
			Arcade::instance.createPost();
		}
		
		return status == PARSEC_OK;

	}
	return false;
}

bool Hosting::isFilteredCommand(ACommand* command) {
	// TODO: Implement a filter for commands
	return false;
}

uint32_t Hosting::ipToUint(const std::string& ip) {
	uint32_t result = 0;
	std::istringstream iss(ip);
	std::string token;
	while (std::getline(iss, token, '.')) {
		result = (result << 8) + std::stoi(token);
	}
	return result;
}

bool Hosting::isIPInRange(const std::string& ip, const std::string& cidr) {
	size_t pos = cidr.find('/');
	std::string base_ip = cidr.substr(0, pos);
	int prefix_len = std::stoi(cidr.substr(pos + 1));

	uint32_t ip_addr = ipToUint(ip);
	uint32_t net_addr = ipToUint(base_ip);

	_chatLog.logMessage("IP: " + ip + " Net: " + base_ip + " Prefix: " + std::to_string(prefix_len));

	uint32_t mask = (prefix_len == 0) ? 0 : ~((1 << (32 - prefix_len)) - 1);

	return (ip_addr & mask) == (net_addr & mask);
}

// Function to check if an IP address is a VPN
bool Hosting::isVPN(const std::string& ip) {
	for (const auto& range : Cache::cache.cidrRanges) {
		if (isIPInRange(ip, range)) {
			return true;
		}
	}
	return false;
}


void Hosting::onGuestStateChange(ParsecGuestState& state, Guest& guest, ParsecStatus& status) {

	static string logMessage;

	/*static string trickDesc = "";
	static Debouncer debouncer = Debouncer(500, [&]() {
		if (_hostConfig.maxGuests > 0 && _guestList.getGuests().size() + 1 == _hostConfig.maxGuests)
		{
			try
			{
				if (trickDesc.size() > 0) trickDesc = "";
				else trickDesc = "-";
				strcpy_s(_hostConfig.desc, trickDesc.c_str());
				applyHostConfig();
			}
			catch (const std::exception&) {}
		}
	});*/

	// Try to determine the user's IP address
	if (Cache::cache.pendingIpAddress.size() > 0) {

		// Is this IP address banned?
		if (Config::cfg.general.ipBan && Cache::cache.isBannedIPAddress(Cache::cache.pendingIpAddress)) {
			broadcastChatMessage(Config::cfg.chatbotName + "Kicked a guest for using a banned IP address.");
			_chatLog.logMessage(Config::cfg.chatbotName + "Kicked a guest for using a banned IP address.");
			ParsecHostKickGuest(_parsec, guest.id);
		} else {
			// Is the user behind a VPN?
			if (isVPN(Cache::cache.pendingIpAddress)) {
				broadcastChatMessage(Config::cfg.chatbotName + " " + guest.name + " is behind a VPN.");
				_chatLog.logMessage(Config::cfg.chatbotName + " " + guest.name + " is behind a VPN.");
				if (Config::cfg.general.blockVPN) {
					ParsecHostKickGuest(_parsec, guest.id);
				}
			}
			else {
				broadcastChatMessage(Config::cfg.chatbotName + " " + guest.name + " is not behind a VPN.");
				_chatLog.logMessage(Config::cfg.chatbotName + " " + guest.name + " is not behind a VPN.");
			}

			Cache::cache.userIpMap[guest.userID] = Cache::cache.pendingIpAddress;
		}
		Cache::cache.pendingIpAddress.clear();
		
	}

	// Is this a fake MickeyUK?
	if ((state == GUEST_CONNECTED || state == GUEST_CONNECTING) && (guest.name == "MickeyUK" && guest.userID != 1693946)) {
		ParsecHostKickGuest(_parsec, guest.id);
		broadcastChatMessage(Config::cfg.chatbotName + "Kicked a fake MickeyUK! (lol)");
		_chatLog.logMessage(Config::cfg.chatbotName + "Kicked a fake MickeyUK! (lol)");
	} else

	// Is the connecting guest the host?
	if ((state == GUEST_CONNECTED || state == GUEST_CONNECTING) && (_host.userID == guest.userID))
	{
		_tierList.setTier(guest.userID, Tier::GOD);
		MetadataCache::addActiveGuest(guest);

		addNewGuest(guest);
	}
	else

	if ((state == GUEST_CONNECTED || state == GUEST_CONNECTING) && Cache::cache.banList.isBanned(guest.userID)) {

		// Yes, but did this person try to ban a SODA COP!?
		if (Cache::cache.isSodaCop(guest.userID)) {
			GuestData unbannedGuest;
			Cache::cache.banList.unban(guest.userID, [&unbannedGuest](GuestData& guest) {
				unbannedGuest = guest;
			});
		}

		ParsecHostKickGuest(_parsec, guest.id);
		logMessage = _chatBot->formatBannedGuestMessage(guest);
		broadcastChatMessage(logMessage);
		_chatLog.logCommand(logMessage);
	}
	else if ((state == GUEST_CONNECTED || state == GUEST_CONNECTING) && Cache::cache.modList.isModded(guest.userID))
	{
		logMessage = _chatBot->formatModGuestMessage(guest);
		broadcastChatMessage(logMessage);
		_tierList.setTier(guest.userID, Tier::MOD);
		_chatLog.logCommand(logMessage);
		MetadataCache::addActiveGuest(guest);
		addNewGuest(guest);
	}
	else if (state == GUEST_FAILED)
	{
		logMessage = _chatBot->formatGuestConnection(guest, state, status);
		broadcastChatMessage(logMessage);
		_chatLog.logCommand(logMessage);
	}
	else if (state == GUEST_CONNECTED || state == GUEST_DISCONNECTED)
	{
		static string guestMsg;
		guestMsg.clear();
		guestMsg = string(guest.name);

		if (Cache::cache.banList.isBanned(guest.userID)) {
			logMessage = _chatBot->formatBannedGuestMessage(guest);
			broadcastChatMessage(logMessage);
			_chatLog.logCommand(logMessage);
			/*if (_hostConfig.maxGuests > 0 && _guestList.getGuests().size() + 1 == _hostConfig.maxGuests)
				debouncer.start();*/
		}
		else
		{
			logMessage = _chatBot->formatGuestConnection(guest, state, status);
			broadcastChatMessage(logMessage);
			_chatLog.logCommand(logMessage);
		}

		if (state == GUEST_CONNECTED) {
			GuestData data = GuestData(guest.name, guest.userID);

			// Is this guest pretending to be someone else?
			if (!Cache::cache.verifiedList.Verify(data)) {
				ParsecHostKickGuest(_parsec, guest.id);
				broadcastChatMessage(Config::cfg.chatbotName + "Kicked a fake guest: " + guest.name);
				_chatLog.logCommand(Config::cfg.chatbotName + "Kicked a fake guest: " + guest.name);
			} else {

				// Add to guest history
				_guestHistory.add(data);
				MetadataCache::addActiveGuest(guest);

				// Show welcome message
				addNewGuest(guest);

			}

		}
		else {
			
			// Were extra spots made?
			if (MetadataCache::preferences.extraSpots > 0) {
				_hostConfig.maxGuests = _hostConfig.maxGuests - 1;
				MetadataCache::preferences.extraSpots--;
				ParsecHostSetConfig(_parsec, &_hostConfig, _parsecSession.sessionId.c_str());
			}

			// Remove from active guests list
			MetadataCache::removeActiveGuest(guest);

			// Hotseat mode
			if (Config::cfg.hotseat.enabled) {
				Hotseat::instance.pauseUser(guest.userID);
			}

			_guestList.deleteMetrics(guest.id);
			int droppedPads = 0;
			CommandFF command(guest, _gamepadClient, _hotseat);
			command.run();
			if (droppedPads > 0) {
				logMessage = command.replyMessage();
				broadcastChatMessage(logMessage);
				_chatLog.logCommand(logMessage);
			}

			try {
				PlaySound(TEXT("./SFX/guest_leave.wav"), NULL, SND_FILENAME | SND_NODEFAULT | SND_ASYNC);
			}
			catch (const std::exception&) {}
			
		}
	}
}

bool Hosting::removeGame(string name) {

	_gamesList.remove(name, [&](GameData& guest) {
		
	});

	return true;

}

void Hosting::logMessage(string message) {
	string chatbotName = Config::cfg.chatbotName;
	_chatLog.logCommand(chatbotName + " " + message);
	if (WebSocket::instance.isRunning()) {
		json j;
		j["event"] = "chat:log";
		j["data"]["message"] = chatbotName + " " + message;
		WebSocket::instance.sendMessageToAll(j.dump());
	}
}

/// <summary>
/// 
/// </summary>
/// <returns></returns>
bool Hosting::isHotseatEnabled() {
	return false;
}

/// <summary>
/// Helper function to add fake guests to the guest list.
/// </summary>
/// <param name="count"></param>
void Hosting::addFakeGuests(int count) {

	_guestList.getGuests().reserve(_guestList.getGuests().size() + count);
	for (int i = 0; i < count; ++i) {
		Guest guest = Guest(
			randomString(5),
			i+1, i+1
		);

		_guestList.getGuests().push_back(guest);
		MetadataCache::addActiveGuest(guest);
	}

}

/// <summary>
/// Helper function to generate a random string.
/// </summary>
string Hosting::randomString(const int len) {

	static const char alphanum[] =
		"0123456789"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz";
	std::string tmp_s;
	tmp_s.reserve(len);

	for (int i = 0; i < len; ++i) {
		tmp_s += alphanum[rand() % (sizeof(alphanum) - 1)];
	}

	return tmp_s;
}

/// <summary>
/// Converts a stick value from short to float.
/// </summary>
/// <param name="lx"></param>
/// <param name="ly"></param>
/// <param name="distance"></param>
ImVec2 Hosting::stickShortToFloat(SHORT lx, SHORT ly, float& distance) {
    static float shortMax = 32768;
    ImVec2 stick = ImVec2(
        (float)lx / shortMax,
        (float)ly / shortMax
    );
    stick.x = min(max(stick.x, -1.0f), 1.0f);
    stick.y = min(max(stick.y, -1.0f), 1.0f);

    distance = sqrtf(stick.x * stick.x + stick.y * stick.y);
    if (distance > 0.01f)
    {
        if (abs(lx) > abs(ly))
        {
            if (abs(stick.x) > 0.01f)
            {
                float yy = stick.y / abs(stick.x);
                float xx = 1.0f;
                float maxDiagonal = sqrtf(xx * xx + yy * yy);
                if (maxDiagonal > 0.01f)
                {
                    stick.x /= maxDiagonal;
                    stick.y /= maxDiagonal;
                }
            }
        }
        else
        {
            if (abs(stick.y) > 0.01f)
            {
                float xx = stick.x / abs(stick.y);
                float yy = 1.0f;
                float maxDiagonal = sqrtf(xx * xx + yy * yy);
                if (maxDiagonal > 0.01f)
                {
                    stick.x /= maxDiagonal;
                    stick.y /= maxDiagonal;
                }
            }
        }
    }

    return stick;
}