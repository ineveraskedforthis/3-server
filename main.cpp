#include <cmath>
#define PI 3.14159265358979323846
#ifndef floorf
namespace std {
	float floorf(float x) { return ::floor(x); }
	float ceilf(float x) { return ::ceil(x); }
}
#endif
#include "iostream"
#include "data.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <unistd.h>
#include <signal.h>
#include <chrono>
#include "unordered_dense.h"
#include <mutex>
#include <thread>


static fd_set udp_singleton;
static fd_set udp_select_singleton;
static fd_set active_connections;
static fd_set read_connections;
static sockaddr_in client_address;
static int connection_address_size;
static struct timeval timeout { 0, 0 };
static int udp_socket;
static int tcp_socket;
static char udp_buffer[1024] = {0};
static std::mutex game_state_mtx {};

static std::default_random_engine rng;
static std::uniform_real_distribution<float> uniform{0.0, 1.0};
static std::normal_distribution<float> normal_d{0.f, 1.f};
static std::normal_distribution<float> size_d{1.f, 0.3f};

void
handle_udp_subscription(
	ankerl::unordered_dense::map<in_addr_t, sockaddr_in>& container
) {
	udp_select_singleton = udp_singleton;

	auto udp_has_message = select(
		FD_SETSIZE,
		&udp_select_singleton,
		NULL,
		NULL,
		&timeout
	);

	if (udp_has_message < 0) {
		perror("Select error (udp)");
		exit(EXIT_FAILURE);
	}

	if (udp_has_message > 0) {
		// handle UDP "subscriptions"
		connection_address_size = sizeof(client_address);
		auto status = recvfrom(udp_socket, udp_buffer, 1024, 0, (struct sockaddr*)&client_address, (socklen_t *) &connection_address_size);

		if (status >= 0) {
			printf("got UDP message\n");
			container[client_address.sin_addr.s_addr] = client_address;
		}
	}
}

constexpr uint8_t MODEL_RAT = 0;
constexpr uint8_t MODEL_HUMAN = 1;
constexpr uint8_t MODEL_UNKNOWN = 255;

constexpr uint8_t WEAPON_TYPE_RAT = 0;
constexpr uint8_t WEAPON_TYPE_HUMAN = 1;
constexpr uint8_t WEAPON_TYPE_KNIFE = 2;

constexpr inline int ACTION_LOGIN = 0;
// sent via tcp
struct action_update {
	int action;
	int player_id;
	int fighter_id;
	int entity_id;
};

void handle_tcp_connection(dcon::data_container& container ) {
	// connection requests
	connection_address_size = sizeof(client_address);

	int new_connection = accept(
		tcp_socket,
		(sockaddr *) & client_address,
		(socklen_t *) &connection_address_size
	);

	if (new_connection < 0) {
		perror("Accept connection error");
		exit(EXIT_FAILURE);
	}

	fprintf(
		stderr,
		"SERVER: NEW CONNECTION\n"
	);

	if (container.player_size() > 100) {
		// Deny connections when there are 100 players
		return;
	}
	auto pid = container.create_player();
	auto fighter = container.create_fighter();
	container.fighter_set_max_hp(fighter, 40);
	container.fighter_set_hp(fighter, 40);
	container.fighter_set_energy(fighter, 1.f);
	container.fighter_set_tx(fighter, 0.f);
	container.fighter_set_ty(fighter, 0.f);
	container.fighter_set_model(fighter, MODEL_HUMAN);
	auto location = container.create_spatial_entity();
	container.force_create_fighter_location(fighter, location);
	container.force_create_player_control(pid, fighter);

	action_update to_send{};
	to_send.action = ACTION_LOGIN;
	to_send.player_id = pid.index();
	to_send.fighter_id = fighter.index();
	to_send.entity_id = location.index();

	send(new_connection, (char*)&to_send, sizeof(action_update), 0);

	container.player_set_connection(pid, new_connection);
	container.player_set_address(pid, client_address.sin_addr.s_addr);
	FD_SET(new_connection, &active_connections);
}

// sent via udp

inline constexpr uint8_t UPDATE_SPATIAL = 0;
inline constexpr uint8_t UPDATE_FIGHTER = 1;
inline constexpr uint8_t UPDATE_RELINK = 2;
inline constexpr uint8_t UPDATE_HIGH_PRECISION = 3;

struct high_precision_update {
	// 4 bytes
	int spatial_entity_id;

	// 4 bytes
	float x;

	// 4 bytes
	float y;
};
static_assert(sizeof(high_precision_update) == 12);

struct spatial_update {
	// 4 bytes
	int spatial_entity_id;

	// 4 bytes
	int16_t x;
	int16_t y;

	// 4 bytes
	uint8_t direction;
	uint8_t padding[3];
};
static_assert(sizeof(spatial_update) == 12);

struct fighter_update {
	// 4 bytes
	int fighter_id;

	// 4 bytes
	int16_t hp;
	int16_t max_hp;

	// 4 bytes
	uint8_t energy;
	uint8_t attack_energy;
	uint8_t model;
	uint8_t weapon_type;
};
static_assert(sizeof(fighter_update) == 12);

struct relink_update {
	// 4 bytes
	int fighter_id;

	// 4 bytes
	int spatial_id;

	// 4 bytes
	uint8_t padding[4];
};
static_assert(sizeof(relink_update) == 12);

struct udp_update {

	// 4 bytes
	uint8_t update_type;
	uint8_t padding[3];

	// 4 bytes
	int timestamp;

	// 4 bytes
	int sent_to_player;

	// 12 bytes
	union {
		spatial_update spatial;
		fighter_update fighter;
		relink_update relink;
		high_precision_update high_precision;
	} payload;
};
static_assert(sizeof(udp_update) == 24);

void
send_network_update_player(
	dcon::data_container& container,
	ankerl::unordered_dense::map<in_addr_t, sockaddr_in>& address_mapping,
	int timestamp,
	dcon::player_id player
) {
	auto connection = container.player_get_connection(player);
	if (!FD_ISSET(connection, &active_connections)) {
		return;
	}

	auto internet_address = container.player_get_address(player);
	auto& udp_address_iterator = address_mapping[internet_address];

	auto player_fighter = container.player_get_controlled_from_player_control(player);
	auto player_location = container.fighter_get_spatial_entity_from_fighter_location(player_fighter);

	{
		// 30 times per second
		udp_update next_update;
		next_update.update_type = UPDATE_HIGH_PRECISION;
		next_update.timestamp = timestamp;
		next_update.sent_to_player = player.index();

		next_update.payload.high_precision.spatial_entity_id = player_location.index();
		next_update.payload.high_precision.x = container.spatial_entity_get_x(player_location);
		next_update.payload.high_precision.y = container.spatial_entity_get_y(player_location);

		sendto(
			udp_socket,
			(char*)&next_update,
			sizeof(next_update),
			0,
			(sockaddr *) &(udp_address_iterator),
			sizeof(next_update)
		);
	}

	container.for_each_spatial_entity([&](auto location){
		auto shift_x = container.spatial_entity_get_x(location) - container.spatial_entity_get_x(player_location);
		auto shift_y = container.spatial_entity_get_y(location) - container.spatial_entity_get_y(player_location);
		if (abs(shift_x) > 80.f) {
			return;
		}
		if (abs(shift_y) > 80.f) {
			return;
		}

		if (abs(shift_x) > 10.f && (timestamp % 3) != 0) {
			return;
		}
		if (abs(shift_y) > 10.f && (timestamp % 3) != 0) {
			return;
		}

		{
			udp_update next_update;
			next_update.update_type = UPDATE_SPATIAL;
			next_update.timestamp = timestamp;
			next_update.sent_to_player = player.index();

			next_update.payload.spatial.spatial_entity_id = location.index();
			next_update.payload.spatial.x = shift_x * 100.f;
			next_update.payload.spatial.y = shift_y * 100.f;
			next_update.payload.spatial.direction = (uint8_t)(container.spatial_entity_get_direction(location) / 2.f / PI * 255);

			sendto(
				udp_socket,
				(char*)&next_update,
				sizeof(next_update),
				0,
				(sockaddr *) &(udp_address_iterator),
				sizeof(next_update)
			);
		}

		auto fighter = container.spatial_entity_get_fighter_from_fighter_location(location);
		if (!fighter) return;

		if ((timestamp % 6) == 0) {
			// 5 times per second
			udp_update next_update;
			next_update.update_type = UPDATE_FIGHTER;
			next_update.timestamp = timestamp;
			next_update.sent_to_player = player.index();

			next_update.payload.fighter.fighter_id = fighter.index();
			next_update.payload.fighter.energy = (uint8_t)(container.fighter_get_energy(fighter) * 255);
			next_update.payload.fighter.attack_energy = (uint8_t)(container.fighter_get_attack_energy_buffer(fighter) * 255);
			next_update.payload.fighter.hp = container.fighter_get_hp(fighter);
			next_update.payload.fighter.max_hp = container.fighter_get_max_hp(fighter);
			next_update.payload.fighter.model = container.fighter_get_model(fighter);
			sendto(
				udp_socket,
				(char*)&next_update,
				sizeof(next_update),
				0,
				(sockaddr *) &(udp_address_iterator),
				sizeof(next_update)
			);
		}

		if ((timestamp % 15) == 0) {
			// 2 times per second
			udp_update next_update;
			next_update.update_type = UPDATE_RELINK;
			next_update.timestamp = timestamp;
			next_update.sent_to_player = player.index();
			next_update.payload.relink.fighter_id = fighter.index();
			next_update.payload.relink.spatial_id = location.index();
			sendto(
				udp_socket,
				(char*)&next_update,
				sizeof(next_update),
				0,
				(sockaddr *) &(udp_address_iterator),
				sizeof(next_update)
			);
		}
	});
}

void
send_network_updates(
	dcon::data_container& container,
	ankerl::unordered_dense::map<in_addr_t, sockaddr_in>& address_mapping,
	int timestamp
) {
	container.for_each_player([&](auto dest) {
		send_network_update_player(container, address_mapping, timestamp, dest);
	});
}

void sigpipe_handler(int unused)
{

}

namespace command {
inline constexpr uint8_t MOVE = 0;
inline constexpr uint8_t RUN_START = 1;
inline constexpr uint8_t RUN_STOP = 2;
inline constexpr uint8_t ATTACK_START = 3;
inline constexpr uint8_t ATTACK_STOP = 4;
inline constexpr uint8_t RESPAWN = 5;

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

int consume_command(dcon::data_container& container, int connection, command::data command) {
	std::lock_guard<std::mutex> lk(game_state_mtx);

	printf("new command %d\n", command.command_type);

	dcon::player_id id { (dcon::player_id::value_base_t) command.actor };

	if (!container.player_is_valid(id)) {
		return 0;
	}

	/*
	We have to check that the player is actually controlled by whoever is connected at the other end
	*/
	if (container.player_get_connection(id) != connection) {
		return 0;
	}

	/*
	Every player has a fighter?
	*/

	auto control = container.player_get_player_control(id);
	auto fighter = container.player_control_get_controlled(control);
	auto location = container.fighter_get_spatial_entity_from_fighter_location(fighter);

	if (
		command.command_type == command::MOVE
	) {
		container.fighter_set_tx(fighter, command.target_x);
		container.fighter_set_ty(fighter, command.target_y);
	} else if (
		command.command_type == command::RUN_START
		&& container.fighter_get_energy(fighter) > 0.2f
	) {
		container.fighter_set_running(fighter, true);
	} else if (
		command.command_type == command::RUN_STOP
	) {
		container.fighter_set_running(fighter, false);
	} else if (
		command.command_type == command::ATTACK_START
		&& container.fighter_get_energy(fighter) > 0.1f
	) {
		container.fighter_set_attacking(fighter, true);
	} else if (
		command.command_type == command::ATTACK_STOP
	) {
		container.fighter_set_attacking(fighter, false);
	} else if (
		command.command_type == command::RESPAWN
	) {
		container.fighter_set_hp(fighter, container.fighter_get_max_hp(fighter));
		container.spatial_entity_set_x(location, 0.f);
		container.spatial_entity_set_y(location, 0.f);
		container.fighter_set_attacking(fighter, false);
	}


	return 0;
}

void clean_player(dcon::data_container& container, int connection) {
	perror("Read failed");
	// connection ended
	// delete players with this connection
	std::vector<dcon::player_id> players_to_delete;
	container.for_each_player([&](auto pid) {
		if (container.player_get_connection(pid) == connection)
			players_to_delete.push_back(pid);
	});


	for (int i = 0; i < (int)players_to_delete.size(); ++i) {
		auto pid = players_to_delete[i];
		auto control = container.player_get_player_control(pid);
		auto fighter = container.player_control_get_controlled(control);
		// event_notification(container, fighter, update::EVENT_LEFT_GAME);
		printf("delete player %d\n", pid.index());
		container.delete_player(pid);
		if (fighter) {
			container.delete_fighter(fighter);
		}
	}
}

int read_from_connection (dcon::data_container& container, int connection) {
	char buffer[256];
	int nbytes;
	nbytes = read(connection, buffer, 256);

	if (nbytes <= 0) {
		clean_player(container, connection);
		return -1;
	} else {
		command::data command {};
		memcpy(&command, buffer, sizeof(command::data));
		return consume_command(container, connection, command);
	}

	return 0;
}

float move_speed_from_wrong_direction(
	dcon::data_container& container,
	dcon::spatial_entity_id fid,
	float dx,
	float dy
) {
	if (dx == 0.f && dy== 0.f) {
		return 1.f;
	}
	auto desired_direction = atan2f(dy, dx);
	auto direction = container.spatial_entity_get_direction(fid);
	return 0.1 + std::max(0.f, cosf(direction - desired_direction));
}

void rotate_toward(dcon::data_container & container, float dt, dcon::spatial_entity_id id, float dx, float dy, float rotation_speed) {
	if (dx != 0 || dy != 0) {
		auto desired_direction = atan2f(dy, dx);
		auto direction = container.spatial_entity_get_direction(id);
		auto diff = fmodf(desired_direction - direction + 4 * PI, 2 * PI);
		if (diff <= rotation_speed * dt) {
			direction = desired_direction;
		} else if (diff <= PI) {
			direction = direction + rotation_speed * dt;
		} else if (diff < 2 * PI - rotation_speed * dt) {
			direction = direction - rotation_speed * dt;
		} else {
			direction = desired_direction;
		}
		direction = fmodf(direction + 2 * PI, 2 * PI);
		container.spatial_entity_set_direction(id, direction);
	}
}

void update_ai_state(dcon::data_container & container) {
	container.for_each_fighter([&](auto fid){
		auto player = container.fighter_get_controller_from_player_control(fid);
		if(player) return;

		// for now: just wander aimlessly
		auto tx = container.fighter_get_tx(fid);
		auto ty = container.fighter_get_ty(fid);
		tx += 0.5f *(uniform(rng) - 0.5f);
		ty += 0.5f *(uniform(rng) - 0.5f);
		auto norm = tx * tx + ty * ty;
		if (norm > 0.f) {
			tx /= norm;
			ty /= norm;
		}
		container.fighter_set_tx(fid, tx);
		container.fighter_set_ty(fid, ty);
	});
}

void update_game_state(dcon::data_container & container, std::chrono::microseconds last_tick) {
	float dt = float(last_tick.count()) / 1'000'000.f;

	float energy_regen_rate = 0.15f;
	float energy_walking_spend = energy_regen_rate * 0.75f;
	float energy_running_spend = energy_regen_rate * 1.2f;
	float attack_energy_siphon = energy_regen_rate * 1.25f;
	float attack_energy_siphon_efficiency = 2.5f;
	float attack_max_energy = 0.125f  * 5.5f;
	float attack_half_angle = PI / 3.f;
	float attack_energy_drain = energy_regen_rate * 0.05f;

	auto attack_energy_decay = expf(-dt / 2.f);

	container.for_each_fighter([&](auto fid){

		auto hp = container.fighter_get_hp(fid);
		if (hp <= 0) {
			return;
		}

		auto position = container.fighter_get_fighter_location(fid);
		auto spatial = container.fighter_location_get_spatial_entity(position);

		float attack_range = 2.f;

		auto x = container.spatial_entity_get_x(spatial);
		auto y = container.spatial_entity_get_y(spatial);
		auto tx = container.fighter_get_tx(fid);
		auto ty = container.fighter_get_ty(fid);
		auto dx = tx;
		auto dy = ty;

		auto energy_gain = dt * energy_regen_rate;
		auto energy_loss = 0.f;

		auto rotation_speed = 4.5f;
		float speed_mod = 3.f * move_speed_from_wrong_direction(container, spatial, dx, dy);

		auto running = container.fighter_get_running(fid) && container.fighter_get_energy(fid) > 0.1f;
		if (running) {
			speed_mod *= 2.5f;
		}

		auto attacking = container.fighter_get_attacking(fid);
		if (attacking) {
			speed_mod *= 0.3f;
			energy_loss += dt * (attack_energy_siphon + attack_energy_drain);
		}

		rotate_toward(container, dt, spatial, dx, dy, rotation_speed);

		auto norm = sqrtf(dx * dx + dy * dy);
		if (norm > 1.f) {
			dx /= norm;
			dy /= norm;
		}

		if (norm > 0.f) {
			if (running) {
				energy_loss += energy_running_spend * dt;
			} else {
				energy_loss += energy_walking_spend * dt;
			}
		}


		auto total_energy_budget = container.fighter_get_energy(fid) + energy_gain;
		auto actual_energy_spending_rate = 1.f;
		if (energy_loss > 0.f && energy_loss > total_energy_budget) {
			actual_energy_spending_rate = total_energy_budget / energy_loss;
		}

		// now we can actually charge attack and move according to available energy

		// moving
		speed_mod *= actual_energy_spending_rate;
		if (container.fighter_get_action_type(fid) == command::MOVE) {
			x = container.spatial_entity_get_x(spatial);
			y = container.spatial_entity_get_y(spatial);
			x += dx * dt * speed_mod;
			y += dy * dt * speed_mod;
			container.spatial_entity_set_x(spatial, x);
			container.spatial_entity_set_y(spatial, y);
		}

		/*
		When player is charging attack, the energy is accumulated in a special buffer.
		When player releases attack, he spends accumulated energy rounded down to the integer to deal damage.
		X units of energy deal X * X damage.
		If a target had accumulated not more than 2 X units of attack energy, then their attack is interrupted.

		Design goals are:
		We want heavy attacks to be more cost efficient, so players want to use them.
		We want players to be able intercept heavier attacks, so they could protect themselves.
		But we also don't want light attack spam to be a valid tactic, so we don't allow light attacks to intercept really heavy attacks
		*/

		auto cashback = 0.f;

		if (attacking) {
			auto siphoned_energy = dt * attack_energy_siphon * attack_energy_siphon_efficiency * actual_energy_spending_rate;
			auto current_energy = container.fighter_get_attack_energy_buffer(fid);
			cashback += std::max(0.f, current_energy * attack_energy_decay + siphoned_energy - attack_max_energy) / attack_energy_siphon_efficiency / actual_energy_spending_rate;
			siphoned_energy = std::min(attack_max_energy - current_energy * attack_energy_decay, siphoned_energy);
			container.fighter_set_attack_energy_buffer(fid, current_energy * attack_energy_decay + siphoned_energy);
		} else {
			auto attack_strength = floorf(container.fighter_get_attack_energy_buffer(fid) / 0.125f);
			auto damage = attack_strength * attack_strength;
			cashback += container.fighter_get_attack_energy_buffer(fid) - attack_strength * 0.125f;
			container.fighter_set_attack_energy_buffer(fid, 0.f);
			if (damage > 0.f) {
				container.for_each_fighter([&](dcon::fighter_id target){
					if (target == fid) return;
					auto spatial_target = container.fighter_get_spatial_entity_from_fighter_location(target);
					auto candidate_x = container.spatial_entity_get_x(spatial_target);
					auto candidate_y = container.spatial_entity_get_y(spatial_target);

					x = container.spatial_entity_get_x(spatial);
					y = container.spatial_entity_get_y(spatial);

					auto target_direction_x = candidate_x - x;
					auto target_direction_y = candidate_y - y;

					auto target_angle = atan2f(target_direction_y, target_direction_x);
					auto actual_angle = container.spatial_entity_get_direction(spatial);

					if (
						abs(target_angle - actual_angle) > attack_half_angle
						&& abs(target_angle - actual_angle + PI *2.f) > attack_half_angle
						&& abs(target_angle - actual_angle - PI *2.f) > attack_half_angle
					) {
						return;
					}

					auto target_distance_squared = target_direction_x * target_direction_x + target_direction_y * target_direction_y;

					if (
						target_distance_squared > attack_range * attack_range
					) {
						return;
					}

					auto hp = container.fighter_get_hp(target);
					container.fighter_set_hp(target, hp - damage);

					auto target_attack_energy = container.fighter_get_attack_energy_buffer(target);
					auto target_attack_strength = floorf(target_attack_energy /  0.125f);

					if (target_attack_strength <= attack_strength * 2.f) {
						container.fighter_set_attack_energy_buffer(target, 0.f);
					}
				});
			}
		}

		container.fighter_set_energy(fid, std::clamp(total_energy_budget + cashback - energy_loss, 0.f, 1.f));
	});
}

dcon::data_container container;

int main(int argc, char const* argv[]) {



	// Spawn a few rats
	for (int count = 0; count < 200; count++) {
		auto x = normal_d(rng) * 10.f;
		auto y = normal_d(rng) * 10.f;

		auto fighter = container.create_fighter();
		container.fighter_set_max_hp(fighter, 100);
		container.fighter_set_hp(fighter, 100);
		container.fighter_set_energy(fighter, 1.f);
		container.fighter_set_tx(fighter, 0.f);
		container.fighter_set_ty(fighter, 0.f);
		container.fighter_set_model(fighter, MODEL_RAT);

		auto location = container.create_spatial_entity();
		container.spatial_entity_set_x(location, x);
		container.spatial_entity_set_y(location, y);
		container.spatial_entity_set_direction(location, PI * uniform(rng) * 2.f);
		container.force_create_fighter_location(fighter, location);
	}


	struct sigaction action { { sigpipe_handler } };
	sigaction(SIGPIPE, &action, NULL);
	if (argc == 1) {
		std::cout << "Port is missing\n";
		exit(EXIT_FAILURE);
	}
	errno = 0;
	const long port = strtol(argv[1], nullptr, 10);
	std::cout << "Attempt to run server at " << port << "\n";

	tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
	if(tcp_socket < 0) {
		perror("TCP socket failed");
		exit(EXIT_FAILURE);
	}
	udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (udp_socket < 0) {
		perror("UDP socket failed");
		exit(EXIT_FAILURE);
	}
	int opt = 1;
	if(setsockopt(
		tcp_socket,
		SOL_SOCKET,
		SO_REUSEADDR | SO_REUSEPORT,
		&opt,
		sizeof(opt)
	)) {
		perror("setsockopt failed");
		exit(EXIT_FAILURE);
	}

	sockaddr_in address;
	socklen_t address_length = sizeof(address);
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);

	if(bind(tcp_socket, (sockaddr *) &address, address_length) < 0) {
		perror("TCP bind failed");
		exit(EXIT_FAILURE);
	}
	if(bind(udp_socket, (sockaddr *) &address, address_length) < 0) {
		perror("UDP bind failed");
		exit(EXIT_FAILURE);
	}
	if(listen(tcp_socket, 5) < 0 ) {
		perror("TCP listen failed");
		exit(EXIT_FAILURE);
	}

	std::cout << "Listening\n";
	FD_ZERO(&active_connections);
	FD_SET(tcp_socket, &active_connections);
	FD_ZERO(&udp_singleton);
	FD_SET(udp_socket, &udp_singleton);
	int i;
	int updated = 0;
	int32_t timestamp = 0;
	ankerl::unordered_dense::map<in_addr_t, sockaddr_in> internet_address_to_udp_address {};


	std::thread updates_of_game_state ([&]() {
		auto now = std::chrono::system_clock::now();
		while (1) {
			auto then = std::chrono::system_clock::now();
			auto duration = std::chrono::duration_cast<std::chrono::microseconds> (
				then - now
			);

			if (duration.count() > 1000 * 1000 / 200) {
				game_state_mtx.lock();
				update_game_state(container, duration);
				now = then;
				game_state_mtx.unlock();
			}
			usleep(10);
		}
	});

	std::thread updates_of_ai_state ([&]() {
		auto now = std::chrono::system_clock::now();
		while (1) {
			auto then = std::chrono::system_clock::now();
			auto duration = std::chrono::duration_cast<std::chrono::microseconds> (
				then - now
			);
			if (duration.count() > 1000 * 1000 / 2) {
				game_state_mtx.lock();
				update_ai_state(container);
				now = then;
				game_state_mtx.unlock();
			}
			usleep(100);
		}
	});


	std::thread network_stuff ([&]() {
		auto now = std::chrono::system_clock::now();
		while (1) {
			handle_udp_subscription(internet_address_to_udp_address);
			auto then = std::chrono::system_clock::now();
			auto duration = std::chrono::duration_cast<std::chrono::microseconds> (
				then - now
			);
			if (duration.count() > 1000 * 1000 / 30) {
				game_state_mtx.lock();
				now = then;
				timestamp++;
				send_network_updates(container, internet_address_to_udp_address, timestamp);
				game_state_mtx.unlock();
			}
			usleep(10);
		}
	});

	// TCP
	while (1) {
		read_connections = active_connections;
		// retrieve sockets which demand attention
		if (updated = select(FD_SETSIZE, &read_connections, NULL, NULL, &timeout); updated < 0) {
			perror("Select error");
			exit(EXIT_FAILURE);
		}
		for (i = 0; i < FD_SETSIZE && updated > 0; ++i) {
			if (!FD_ISSET(i, &read_connections)) {
				continue;
			}
			if (i == tcp_socket) {
				handle_tcp_connection(container);
			} else {
				// data from established connection
				if (read_from_connection(container, i) < 0) {
					// invalid data
					printf("close %d\n", i);
					close(i);
					FD_CLR(i, &active_connections);
				}
			}
		}
		usleep(10);
	}
}