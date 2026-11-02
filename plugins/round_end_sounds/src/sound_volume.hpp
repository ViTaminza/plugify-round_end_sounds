// round_end_sounds — per-player volume of a started soundevent (the SoundVolume module of the
// original; each plugin carries its own copy, no shared wrapper library).
//
// CS2 ignores EmitSound_t::m_flVolume for soundevents, so the original watched
// IGameEventSystem::PostEventAbstract for GE_SosStartSoundEvent (208) and, for every recipient,
// immediately sent him a GE_SosSetSoundEventParams (210) naming the same soundevent_guid with a
// "volume" parameter. This port does the same through s2sdk's user-message API.
//
// HARD RULE (network-message ids are not stable across game builds): every message type is
// resolved by its REAL registry substring on a live map - "SosStartSoundEvent",
// "SosSetSoundEventParams" and "SosStopSoundEvent". s2sdk's UserMessageCreateFromId() does NOT
// return null for an unknown id: it constructs an object whose m_msgSerializable/m_msg are null,
// and GetMessageName/GetMessageID/Pb*/Send then dereference them. Therefore:
//   * no hard-coded 208/209/210 fallback is ever used;
//   * an unresolved type makes exactly that functionality a no-op (fail soft);
//   * UserMessageCreateFromId()/Pb*/Send are only called with an id that the registry lookup
//     resolved by name on this live map;
//   * UserMessageGetMessageName() is only used to verify an INCOMING, engine-owned message (which
//     is valid by construction), never on an object this module created.
//
// Binding availability is checked once in Init(); id resolution is retried lazily on live maps.

#pragma once

#include <cstdint>

namespace sound_volume {

// Checks the s2sdk bindings this module needs. No message name is resolved before a map is up.
bool Init();
// Drops every id/binding flag again (OnPluginEnd / a `plg reload` followed by OnPluginStart).
void Reset();

// Resolves each still-unresolved message type by its real registry name. Safe to call on live
// maps only; a failed lookup leaves that type unresolved (s2sdk logs the lookup failure). Returns
// true when the start type is resolved.
bool ResolveIds();
// Per-type state. A false answer means the corresponding functionality must stay a no-op.
bool StartResolved();
bool SetResolved();
bool StopResolved();

// Message ids resolved by name; -1 while that type is unresolved.
int StartMessageId();
int SetMessageId();
int StopMessageId();

// s2sdk::UserMessageCallback payload -> (guid, soundevent hash). False when it is not a sos start
// or the message carries no soundevent_guid (the field is optional in CMsgSosStartSoundEvent).
bool ParseStart(void* pUserMessage, int& iGuid, uint32_t& uHash);

// Volume (1.0 = the soundevent's own) of sound iGuid for one player, and the stop counterpart.
// Both are no-ops unless the Set/Stop type was resolved by name.
void Set(int iSlot, int iGuid, float flVolume);
void Stop(int iSlot, int iGuid);

// Recipients of the hooked message as a bit mask (0 when the message has none).
uint64_t RecipientMask(void* pUserMessage);

} // namespace sound_volume
