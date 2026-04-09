#pragma once
#include <cstdint>

namespace command {

inline constexpr uint8_t MOVE = 0;
inline constexpr uint8_t SPELL = 1;
inline constexpr uint8_t SELECTION = 2;
inline constexpr uint8_t PARRY = 3;
inline constexpr uint8_t SELECT_CLASS = 4;
inline constexpr uint8_t JOIN_LOBBY = 5;
inline constexpr uint8_t KNOW_MYSELF = 6;
inline constexpr uint8_t ATTACK = 7;
inline constexpr uint8_t INVISIBILITY_PREPARATION = 8;
inline constexpr uint8_t CHARGE_PREPARATION = 9;
inline constexpr uint8_t KNOW_MY_BODY = 10;
inline constexpr uint8_t JUMP_BEHIND = 11;

inline constexpr uint8_t CLASS_MAGE = 0;
inline constexpr uint8_t CLASS_WARRIOR = 1;
inline constexpr uint8_t CLASS_ROGUE = 2;
inline constexpr uint8_t CLASS_TOTAL = 3;

struct data {
	int32_t actor;
	int32_t target_actor;
	float target_x;
	float target_y;
	uint8_t command_type;
	uint8_t command_data;
	uint8_t padding[2];
};

}

static_assert(sizeof(command::data) == 4 * 5);

namespace update {
inline constexpr uint8_t FIGHTER = 0;
inline constexpr uint8_t SPELL = 1;
inline constexpr uint8_t SEND_ID = 2;
inline constexpr uint8_t EVENT = 3;
inline constexpr uint8_t SEND_FIGHTER_ID = 4;

inline constexpr uint8_t EVENT_NONE = 0;
inline constexpr uint8_t EVENT_START_CAST = 1;
inline constexpr uint8_t EVENT_START_PARRY = 2;
inline constexpr uint8_t EVENT_NO_DAMAGE = 3;
inline constexpr uint8_t EVENT_JOIN_LOBBY = 4;
inline constexpr uint8_t EVENT_JOIN_BATTLE = 5;
inline constexpr uint8_t EVENT_LEFT_GAME = 6;
inline constexpr uint8_t EVENT_START_ATTACK = 7;
inline constexpr uint8_t EVENT_START_INVISIBILITY_PREPARATION = 8;
inline constexpr uint8_t EVENT_START_INVISIBILITY = 9;
inline constexpr uint8_t EVENT_STUN = 10;
inline constexpr uint8_t EVENT_START_CHARGE = 11;
inline constexpr uint8_t EVENT_PLAYER_DIED = 12;

struct data {
	int32_t id;
	float x;
	float y;
	uint8_t update_type;
	uint8_t belongs_to;
	uint8_t additional_data;
	uint8_t event_type;
};

struct udp_data {
	int32_t timestamp;
	int32_t id;
	float x;
	float y;
	float z;
	uint8_t update_type;
	uint8_t belongs_to;
	uint8_t additional_data;
	uint8_t event_type;
	int32_t flags;
};
}

static_assert(sizeof(update::data) == 4 * 4);
static_assert(sizeof(update::udp_data) == 7 * 4);

constexpr int buffer_size = 256;

static_assert(sizeof(command::data) < buffer_size);
static_assert(sizeof(update::data) < buffer_size);
static_assert(sizeof(update::udp_data) < buffer_size);