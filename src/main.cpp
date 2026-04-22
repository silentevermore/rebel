#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include <unistd.h>
#include <atomic>
#include <fstream>
#include <sstream>
#include <mutex>
#include <chrono>

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/select.h>

#include <ncurses.h>

const std::string HISTORY_FILE = "chat_history.log";
const std::string SECRET_KEY = "REBEL_P2P_SECRET_HANDSHAKE_0X22";

std::atomic<bool> connected{false};
std::atomic<bool> listening{false};
std::atomic<std::chrono::steady_clock::time_point> last_seen;

struct sockaddr_in remote_peer_addr;
int global_sock = -1;
std::atomic<int> active_upnp_port{-1};

WINDOW *chat_bg, *chat_text, *input_bg, *input_text;
std::mutex ui_mutex;

void cleanup_upnp() {
    int port = active_upnp_port.load();
    if (port == -1) return;

    int error = 0;
    struct UPNPDev * devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
    if (!devlist) return;

    struct UPNPUrls urls;
    struct IGDdatas data;
    char lanaddr[64] = {0};
    char wanaddr[64] = {0};

    int status = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));
    if (status == 1 || status == 2) {
        std::string port_str = std::to_string(port);
        int r = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype,
                                       port_str.c_str(), "UDP", nullptr);
        
        if (r == UPNPCOMMAND_SUCCESS) {
			std::cout << "Closed UPnP for port " << active_upnp_port.load() << std::endl;
        }
        FreeUPNPUrls(&urls);
    }
    freeUPNPDevlist(devlist);
}

void display_message(const std::string& user, const std::string& msg, int color_pair, bool save_to_file = true) {
	std::lock_guard<std::mutex> lock(ui_mutex);

	int cur_y, cur_x;
	getyx(input_text, cur_y, cur_x);

	wattron(chat_text, COLOR_PAIR(color_pair));
	wprintw(chat_text, "%s: ", user.c_str());
	wattroff(chat_text, COLOR_PAIR(color_pair));
	wprintw(chat_text, "%s\n", msg.c_str());

	wnoutrefresh(chat_text);
	wnoutrefresh(input_bg);
	wnoutrefresh(input_text);

	wmove(input_text, cur_y, cur_x);

	doupdate();

	if (save_to_file) {
		std::ofstream file(HISTORY_FILE, std::ios::app);
		if (file.is_open()) {
			file << user << "\t" << msg << "\n";
		}
	}
}

void check_timeout() {
	while (listening.load()) {
		if (connected.load()) {
			auto now = std::chrono::steady_clock::now();
			auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - last_seen.load()).count();
			if (diff > 15) {
				connected.store(false);
				display_message("System", "Peer timed out. Waiting for new connection...", 3);
			}
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
}

std::string get_ip_via_http() {
	char buffer[128];
	std::string result = "";
	FILE* pipe = popen("curl -s https://api.ipify.org", "r");
	if (!pipe) return "ERROR";
	while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
		result += buffer;
	}
	pclose(pipe);
	return result;
}

std::string get_ext_addr_via_stun(int sock) {
	struct sockaddr_in servaddr;
	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(19302);
	struct hostent *server = gethostbyname("stun.l.google.com");
	if (!server) return "ERROR";
	memcpy(&servaddr.sin_addr.s_addr, server->h_addr, server->h_length);

	// Magic cookie: 0x21 0x12 0xA4 0x42
	unsigned char stun_request[] = {
		0x00, 0x01, 0x00, 0x00, // Binding Request
		0x21, 0x12, 0xA4, 0x42, // Magic Cookie
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c // Transaction ID
	};

	sendto(sock, stun_request, sizeof(stun_request), 0, (struct sockaddr *)&servaddr, sizeof(servaddr));

	unsigned char buffer[512];
	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);

	struct timeval tv = {2, 0}; 
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	int n = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&from, &fromlen);

	tv = {0, 0}; 
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	if (n >= 20) {
		int i = 20;
		while (i < n - 3) {
			uint16_t attr_type = (buffer[i] << 8) | buffer[i+1];
			uint16_t attr_len  = (buffer[i+2] << 8) | buffer[i+3];

			if (attr_type == 0x0001 && i + 4 + attr_len <= n) { 
				int ext_port = (buffer[i+6] << 8) | buffer[i+7];
				char ip_str[INET_ADDRSTRLEN];
				if (inet_ntop(AF_INET, &buffer[i+8], ip_str, INET_ADDRSTRLEN)) {
					return std::string(ip_str) + ":" + std::to_string(ext_port);
				}
			}
			else if (attr_type == 0x0020 && i + 4 + attr_len <= n) { 
				int ext_port = ((buffer[i+6] ^ 0x21) << 8) | (buffer[i+7] ^ 0x12);
				
				unsigned char ip_bytes[4];
				ip_bytes[0] = buffer[i+8] ^ 0x21;
				ip_bytes[1] = buffer[i+9] ^ 0x12;
				ip_bytes[2] = buffer[i+10] ^ 0xA4;
				ip_bytes[3] = buffer[i+11] ^ 0x42;

				char ip_str[INET_ADDRSTRLEN];
				if (inet_ntop(AF_INET, ip_bytes, ip_str, INET_ADDRSTRLEN)) {
					return std::string(ip_str) + ":" + std::to_string(ext_port);
				}
			}

			i += 4 + attr_len; 
		}
	}
	return "UNKNOWN";
}

void setup_upnp(int port) {
	int error = 0;
	struct UPNPDev * devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
	if (!devlist) return;

	struct UPNPUrls urls;
	struct IGDdatas data;
	char lanaddr[64] = {0};
	char wanaddr[64] = {0};

	int status = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));
	if (status == 1 || status == 2) {
		std::string port_str = std::to_string(port);
		UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
				port_str.c_str(), port_str.c_str(), lanaddr, 
				"REBEL-P2P", "UDP", nullptr, "60");
		FreeUPNPUrls(&urls);
	}
	freeUPNPDevlist(devlist);
}

void udp_listen_loop() {
	char buffer[2048];
	struct sockaddr_in src_addr;
	socklen_t addr_len = sizeof(src_addr);

	while (listening.load()) {
		memset(buffer, 0, sizeof(buffer));
		ssize_t n = recvfrom(global_sock, buffer, sizeof(buffer) - 1, 0, 
				(struct sockaddr*)&src_addr, &addr_len);

		if (n > 0) {
			buffer[n] = '\0';
			last_seen.store(std::chrono::steady_clock::now());
			std::string msg(buffer);
			if (msg.find("PUNCH:") == 0) {
				if (msg.substr(6) == SECRET_KEY) {
					remote_peer_addr = src_addr;
					if (!connected.load()) {
						connected.store(true);
						sendto(global_sock, "PUNCH_ACK", 9, 0, (struct sockaddr*)&src_addr, addr_len);
						display_message("System", "Handshake received! Connected.", 3);
					}
				}
			} 
			else if (msg == "PUNCH_ACK") {
				remote_peer_addr = src_addr;
				if (!connected.load()) {
					connected.store(true);
					display_message("System", "Handshake ACK! Connected.", 3);
				}
			}
			else if (connected.load()) {
				remote_peer_addr = src_addr;
				display_message("Friend", msg, 1); 
			}   
		}
	}
}

void punch_and_connect(std::string ip, int port) {
	struct sockaddr_in target_addr;
	memset(&target_addr, 0, sizeof(target_addr));
	target_addr.sin_family = AF_INET;
	target_addr.sin_port = htons(port);
	inet_pton(AF_INET, ip.c_str(), &target_addr.sin_addr);

	std::string punch_msg = "PUNCH:" + SECRET_KEY;
	display_message("System", "Punching hole to " + ip + ":" + std::to_string(port), 3);

	for (int i = 0; i < 15; ++i) {
		if (connected.load()) break;
		sendto(global_sock, punch_msg.c_str(), punch_msg.length(), 0, 
				(struct sockaddr*)&target_addr, sizeof(target_addr));
		usleep(800000);
	}
	if (!connected.load()) display_message("System", "No response yet, keeping socket open...", 3);
}

void handle_command(const std::string& raw_input) {
	std::stringstream ss(raw_input);
	std::string cmd, arg1, arg2;
	ss >> cmd >> arg1 >> arg2;

	if (cmd == "/listen") {
		if (arg1.empty()) {
			display_message("System", "Usage: /listen <port>", 3);
			return;
		}
		if (listening.load()) {
			display_message("System", "Already listening!", 3);
			return;
		}

		int port = std::stoi(arg1);
		global_sock = socket(AF_INET, SOCK_DGRAM, 0);
		struct sockaddr_in my_addr;
		my_addr.sin_family = AF_INET;
		my_addr.sin_port = htons(port);
		my_addr.sin_addr.s_addr = INADDR_ANY;

		if (bind(global_sock, (struct sockaddr*)&my_addr, sizeof(my_addr)) < 0) {
			display_message("System", "Bind failed! Try another port.", 3);
			return;
		}

		// 1. СНАЧАЛА получаем STUN (пока никто не перехватывает пакеты)
		display_message("System", "Resolving public IP via STUN...", 3);
		std::string my_id = get_ext_addr_via_stun(global_sock);

		if (my_id == "UNKNOWN") {
			display_message("System", "STUN failed. Trying HTTP backup...", 3);
			std::string fallback_ip = get_ip_via_http();
			if (fallback_ip != "ERROR") {
				my_id = fallback_ip + ":" + arg1;
				display_message("System", "Warning: Using HTTP IP. Port might be inaccurate if NAT is strict.", 3);
			}
		}

		// 2. ТЕПЕРЬ запускаем слушающие потоки
		listening.store(true);
		std::thread(udp_listen_loop).detach();
		std::thread(check_timeout).detach();
		
		// 3. UPnP запускаем в фоне, чтобы не морозить интерфейс ncurses
		active_upnp_port.store(port);
		std::thread(setup_upnp, port).detach();

		display_message("System", "Listening on port " + arg1, 3);
		display_message("System", "YOUR PUBLIC ID: " + my_id, 3);
	} else if (cmd == "/connect") {
		if (!listening.load()) {
			display_message("System", "Run /listen first!", 3);
			return;
		}
		if (arg1.empty() || arg2.empty()) {
			display_message("System", "Usage: /connect <ip> <port>", 3);
			return;
		}
		std::thread(punch_and_connect, arg1, std::stoi(arg2)).detach();
	} else {
		display_message("System", "Unknown command or not connected.", 3);
	}
}

void start_gui() {
	initscr();
	start_color();
	init_pair(1, COLOR_RED, COLOR_BLACK);   
	init_pair(2, COLOR_MAGENTA, COLOR_BLACK); 
	init_pair(3, COLOR_CYAN, COLOR_BLACK);    

	int max_y, max_x;
	getmaxyx(stdscr, max_y, max_x);

	chat_bg = newwin(max_y - 3, max_x, 0, 0);
	input_bg = newwin(3, max_x, max_y - 3, 0);
	chat_text = derwin(chat_bg, max_y - 5, max_x - 2, 1, 1);
	input_text = derwin(input_bg, 1, max_x - 2, 1, 1);

	scrollok(chat_text, TRUE);
    keypad(input_text, TRUE);
    nodelay(input_text, TRUE);
    
    box(chat_bg, 0, 0);
    box(input_bg, 0, 0);
    
    display_message("System", "Welcome! Use /listen <port> to start.", 3);

    std::string current_input = "";
    while (true) {
        int ch = wgetch(input_text);

        if (ch != ERR) {
            if (ch == '\n') {
                if (current_input == "/quit") break;
                
                if (current_input.find("/") == 0) {
                    handle_command(current_input);
                } else if (!current_input.empty()) {
                    if (connected.load()) {
                        sendto(global_sock, current_input.c_str(), current_input.length(), 0, 
                               (struct sockaddr*)&remote_peer_addr, sizeof(remote_peer_addr));
                        display_message("You", current_input, 2);
                    } else {
                        display_message("System", "Not connected.", 3);
                    }
                }
                current_input.clear();
                wclear(input_text);
            } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
                if (!current_input.empty()) current_input.pop_back();
            } else if (isprint(ch)) {
                current_input += (char)ch;
            }

            wclear(input_text);
            mvwprintw(input_text, 0, 0, "> %s", current_input.c_str());
            wrefresh(input_text);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    endwin();
}

int main() {
	start_gui();
	cleanup_upnp();
	if (global_sock != -1) close(global_sock);
	return 0;
}
