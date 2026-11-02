// round_end_sounds — sound_volume over s2sdk user messages (see sound_volume.hpp).
#include "sound_volume.hpp"

#include <string>

#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include "re_core.hpp"

namespace sound_volume {
namespace {

// The real registry substrings (the engine registers the user messages without the CMsg/GE_
// prefix). There is deliberately no numeric fallback: an unknown id makes
// UserMessageCreateFromId() return an object with null internals, and the s2sdk getters/setters
// dereference them (user_message.hpp:114-130).
constexpr const char* kStartName = "SosStartSoundEvent";
constexpr const char* kSetName = "SosSetSoundEventParams";
constexpr const char* kStopName = "SosStopSoundEvent";

int g_iStartId = -1;
int g_iSetId = -1;
int g_iStopId = -1;
bool g_bAvailable = false;

int FindByName(const char* szName)
{
	if (!__s2sdk_UserMessageFindMessageIdByName || szName == nullptr) return -1;
	try
	{
		const int id = (int)s2sdk::UserMessageFindMessageIdByName(plg::string(szName));
		return id > 0 ? id : -1;
	}
	catch (...)
	{
		return -1;
	}
}

// One parameter-set message to a single slot. Only reached when SetMessageId() was resolved by
// name on a live map.
void SendParamsTo(int iSlot, int iGuid, float flVolume)
{
	if (!g_bAvailable || g_iSetId <= 0 || iSlot < 0 || iSlot >= 64) return;

	void* pMsg = s2sdk::UserMessageCreateFromId((int16_t)g_iSetId);
	if (!pMsg) return;

	try
	{
		s2sdk::PbSetInt32(pMsg, "soundevent_guid", iGuid);
		s2sdk::PbSetString(pMsg, "packed_params", plg::string(res::PackVolumeParam(flVolume)));
		s2sdk::UserMessageSetRecipientMask(pMsg, 1ull << iSlot);
		s2sdk::UserMessageSend(pMsg);
	}
	catch (...)
	{
	}
	s2sdk::UserMessageDestroy(pMsg);
}

} // namespace

bool Init()
{
	// The generated wrappers call these globals; in a process without the s2sdk plugin they are
	// null. Check the exact set this module uses.
	g_bAvailable = __s2sdk_UserMessageFindMessageIdByName && __s2sdk_UserMessageCreateFromId
		&& __s2sdk_UserMessageSend && __s2sdk_UserMessageDestroy && __s2sdk_UserMessageGetMessageID
		&& __s2sdk_UserMessageGetRecipientMask && __s2sdk_UserMessageSetRecipientMask
		&& __s2sdk_PbReadInt32 && __s2sdk_PbReadUInt32 && __s2sdk_PbSetInt32 && __s2sdk_PbSetString
		&& __s2sdk_UserMessageHasField && __s2sdk_HookUserMessage && __s2sdk_UnhookUserMessage;
	g_iStartId = -1;
	g_iSetId = -1;
	g_iStopId = -1;
	return g_bAvailable;
}

void Reset()
{
	g_bAvailable = false;
	g_iStartId = -1;
	g_iSetId = -1;
	g_iStopId = -1;
}

bool ResolveIds()
{
	if (!g_bAvailable) return false;
	// Resolve each type independently and only once it succeeds; a failed type is retried on the
	// next live map. No numeric fallback is ever substituted.
	if (g_iStartId < 0) g_iStartId = FindByName(kStartName);
	if (g_iSetId < 0) g_iSetId = FindByName(kSetName);
	if (g_iStopId < 0) g_iStopId = FindByName(kStopName);
	return g_iStartId > 0;
}

bool StartResolved() { return g_bAvailable && g_iStartId > 0; }
bool SetResolved() { return g_bAvailable && g_iSetId > 0; }
bool StopResolved() { return g_bAvailable && g_iStopId > 0; }

int StartMessageId() { return StartResolved() ? g_iStartId : -1; }
int SetMessageId() { return SetResolved() ? g_iSetId : -1; }
int StopMessageId() { return StopResolved() ? g_iStopId : -1; }

bool ParseStart(void* pUserMessage, int& iGuid, uint32_t& uHash)
{
	iGuid = -1;
	uHash = 0;
	if (!StartResolved() || !pUserMessage) return false;

	// The incoming message is engine-owned and valid. The id was resolved by name at hook time.
	if ((int)s2sdk::UserMessageGetMessageID(pUserMessage) != g_iStartId) return false;

	// soundevent_guid / soundevent_hash are optional scalars of CMsgSosStartSoundEvent: the last
	// argument of PbRead* is the *repeated* index (-1 for non-repeated fields). With an index >= 0
	// the read fails (s2sdk logs "Field ... is not repeated as expected" on every sound event) and
	// returns 0, which used to be taken for a valid guid.
	if (!s2sdk::UserMessageHasField(pUserMessage, "soundevent_guid")) return false;

	iGuid = s2sdk::PbReadInt32(pUserMessage, "soundevent_guid", -1);
	uHash = s2sdk::PbReadUInt32(pUserMessage, "soundevent_hash", -1);
	return iGuid >= 0;
}

uint64_t RecipientMask(void* pUserMessage)
{
	if (!g_bAvailable || !pUserMessage) return 0;
	try
	{
		return s2sdk::UserMessageGetRecipientMask(pUserMessage);
	}
	catch (...)
	{
		return 0;
	}
}

void Set(int iSlot, int iGuid, float flVolume)
{
	if (!SetResolved()) return; // no CreateFromId with an unverified id
	SendParamsTo(iSlot, iGuid, flVolume);
}

void Stop(int iSlot, int iGuid)
{
	if (!StopResolved() || iSlot < 0 || iSlot >= 64) return; // no CreateFromId with an unverified id

	void* pMsg = s2sdk::UserMessageCreateFromId((int16_t)g_iStopId);
	if (!pMsg) return;

	try
	{
		s2sdk::PbSetInt32(pMsg, "soundevent_guid", iGuid);
		s2sdk::UserMessageSetRecipientMask(pMsg, 1ull << iSlot);
		s2sdk::UserMessageSend(pMsg);
	}
	catch (...)
	{
	}
	s2sdk::UserMessageDestroy(pMsg);
}

} // namespace sound_volume
