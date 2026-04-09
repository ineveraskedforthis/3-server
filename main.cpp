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
	container.fighter_set_tx(fighter, 0.f);
	container.fighter_set_ty(fighter, 0.f);
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
struct position_update {
	int timestamp;
	int destination_player;
	int spatial_entity_id;
	float x;
	float y;
	float direction;
	float speed;
};


void send_position(
	dcon::data_container& container,
	sockaddr_in& target_address,
	int timestamp,
	position_update& to_send,
	dcon::spatial_entity_id seid
) {
	to_send.spatial_entity_id = seid.index();
	to_send.x = container.spatial_entity_get_x(seid);
	to_send.y = container.spatial_entity_get_y(seid);
	to_send.direction = container.spatial_entity_get_direction(seid);
	to_send.speed = container.spatial_entity_get_speed(seid);
	sendto(
		udp_socket,
		(char*)&to_send,
		sizeof(position_update),
		0,
		(sockaddr *) &(target_address),
		sizeof(client_address)
	);
}

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

	// auto control_id = container.player_get_player_control(player);
	// auto controlled_id = container.player_control_get_controlled(control_id);
	auto internet_address = container.player_get_address(player);
	auto& udp_address_iterator = address_mapping[internet_address];

	position_update to_send {};
	to_send.timestamp = timestamp;

	container.for_each_spatial_entity([&](auto location){
		send_position(
			container,
			udp_address_iterator,
			timestamp,
			to_send,
			location
		);
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

	if (
		command.command_type == command::MOVE
	) {
		container.fighter_set_tx(fighter, command.target_x);
		container.fighter_set_ty(fighter, command.target_y);
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

void update_game_state(dcon::data_container & container, std::chrono::microseconds last_tick) {
	float dt = float(last_tick.count()) / 1'000'000.f;

	container.for_each_fighter([&](auto fid){
		auto position = container.fighter_get_fighter_location(fid);
		auto spatial = container.fighter_location_get_spatial_entity(position);

		auto x = container.spatial_entity_get_x(spatial);
		auto y = container.spatial_entity_get_y(spatial);
		auto tx = container.fighter_get_tx(fid);
		auto ty = container.fighter_get_ty(fid);
		auto dx = tx;
		auto dy = ty;

		auto rotation_speed = 4.5f;
		float speed_mod = 0.7f * move_speed_from_wrong_direction(container, spatial, dx, dy);

		rotate_toward(container, dt, spatial, dx, dy, rotation_speed);

		auto norm = sqrtf(dx * dx + dy * dy);
		if (norm > 1.f) {
			dx /= norm;
			dy /= norm;
		}

		if (container.fighter_get_action_type(fid) == command::MOVE) {
			x = container.spatial_entity_get_x(spatial);
			y = container.spatial_entity_get_y(spatial);
			x += dx * dt * speed_mod;
			y += dy * dt * speed_mod;
			container.spatial_entity_set_x(spatial, x);
			container.spatial_entity_set_y(spatial, y);
		}
	});
}

dcon::data_container container;

int main(int argc, char const* argv[]) {
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