#define SAMPLE_RATE 48000
#define FRAME_SIZE 960 // 20ms при 48kHz
#define CHANNELS 1

#include <queue>
#include <vector>
#include <condition_variable>
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

#include <opus/opus.h>

#include <pulse/simple.h>
#include <pulse/error.h>

#include <ncurses.h>

struct AudioPacket {
    char type = 0x02;
    unsigned char data[512];
};

const std::string HISTORY_FILE = "chat_history.log";
const std::string SECRET_KEY = "REBEL_P2P_SECRET_HANDSHAKE_0X22";

std::atomic<bool> connected{false};
std::atomic<bool> listening{false};
std::atomic<std::chrono::steady_clock::time_point> last_seen;

std::atomic<bool> my_voice_on{false};
std::string my_nick = "Anon";
std::atomic<bool> peer_voice_on{false};

std::atomic<int> max_x{0};
std::atomic<int> max_y{0};

struct sockaddr_in remote_peer_addr;
int global_sock = -1;
std::atomic<int> active_upnp_port{-1};

std::queue<std::vector<unsigned char>> audio_queue;
std::mutex audio_queue_mutex;
std::condition_variable audio_queue_cv;
const size_t MAX_AUDIO_QUEUE_SIZE = 15;

WINDOW *chat_bg, *chat_text, *input_bg, *input_text;
std::mutex ui_mutex;

OpusDecoder *opus_decoder = nullptr;
pa_simple *pa_capture = nullptr;
pa_simple *pa_playback = nullptr;

void init_voice_rx() {
	std::string app_name = "RebelP2P_" + std::to_string(active_upnp_port);

	pa_sample_spec ss;
	ss.format = PA_SAMPLE_S16LE;
	ss.channels = CHANNELS;
	ss.rate = SAMPLE_RATE;

	pa_buffer_attr attr;
	attr.maxlength = pa_usec_to_bytes(50 * 1000, &ss);
	attr.tlength = pa_usec_to_bytes(20 * 1000, &ss);
	attr.prebuf = 0;
	attr.minreq = pa_usec_to_bytes(10 * 1000, &ss);
	attr.fragsize = pa_usec_to_bytes(20 * 1000, &ss);

    int error;
	pa_playback = pa_simple_new(NULL, app_name.c_str(), PA_STREAM_PLAYBACK, NULL, "playback", &ss, NULL, &attr, &error);
    
    int opus_err;
    opus_decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &opus_err);
}

void voice_capture_thread() {
	std::string rec_name = "RebelP2P_Rec_" + std::to_string(active_upnp_port);

	pa_sample_spec ss;
	ss.format = PA_SAMPLE_S16LE;
	ss.channels = CHANNELS;
	ss.rate = SAMPLE_RATE;

	pa_buffer_attr attr;
	attr.maxlength = pa_usec_to_bytes(50 * 1000, &ss);
	attr.tlength = pa_usec_to_bytes(20 * 1000, &ss);
	attr.prebuf = 0;
	attr.minreq = pa_usec_to_bytes(10 * 1000, &ss);
	attr.fragsize = pa_usec_to_bytes(20 * 1000, &ss);

    int error;
    pa_capture = pa_simple_new(NULL, rec_name.c_str(), PA_STREAM_RECORD, NULL, "capture", &ss, NULL, &attr, &error);
    
    OpusEncoder *encoder;
    encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_VOIP, &error);

    int16_t pcm_buffer[FRAME_SIZE];
    unsigned char compressed[512];

    while (listening.load()) {
        if (my_voice_on.load() && connected.load() && pa_capture) {
            if (pa_simple_read(pa_capture, pcm_buffer, sizeof(pcm_buffer), &error) < 0) continue;

            int bytes_encoded = opus_encode(encoder, pcm_buffer, FRAME_SIZE, compressed + 1, 511);
            if (bytes_encoded > 0) {
                compressed[0] = 0x02;
                sendto(global_sock, compressed, bytes_encoded + 1, 0, (struct sockaddr*)&remote_peer_addr, sizeof(remote_peer_addr));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    if (pa_capture) pa_simple_free(pa_capture);
}

void voice_playback_thread() {
    while (listening.load()) {
        std::vector<unsigned char> packet;
        {
            std::unique_lock<std::mutex> lock(audio_queue_mutex);
            audio_queue_cv.wait_for(lock, std::chrono::milliseconds(100), []{ 
                return !audio_queue.empty() || !listening.load(); 
            });

            if (!listening.load()) break;
            if (audio_queue.empty()) continue;

            packet = audio_queue.front();
            audio_queue.pop();
        }

        if (peer_voice_on.load() && opus_decoder && pa_playback) {
            int16_t out_pcm[FRAME_SIZE];
            int samples = opus_decode(opus_decoder, packet.data(), packet.size(), out_pcm, FRAME_SIZE, 0);
            
            if (samples > 0) {
                int pa_err;
                pa_simple_write(pa_playback, out_pcm, samples * sizeof(int16_t), &pa_err);
            }
        }
    }
}

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

void refresh_status_bar() {
    std::lock_guard<std::mutex> lock(ui_mutex);
    int cur_y, cur_x;
    getyx(input_text, cur_y, cur_x);

    mvwprintw(chat_bg, 0, 2, "[ Voice: %s ]", my_voice_on ? "ON " : "OFF");
    mvwprintw(chat_bg, 0, max_x.load() - 22, "[ Peer Voice: %s ]", peer_voice_on ? "ON " : "OFF");
    
    wnoutrefresh(chat_bg);
    wmove(input_text, cur_y, cur_x);
    wnoutrefresh(input_text);
    doupdate();
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

void heartbeat_loop() {
    while (listening.load()) {
        if (connected.load()) {
            std::string ping = "KEEPALIVE_PING";
            sendto(global_sock, ping.c_str(), ping.length(), 0, 
                   (struct sockaddr*)&remote_peer_addr, sizeof(remote_peer_addr));
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
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
        ssize_t n = recvfrom(global_sock, buffer, sizeof(buffer) - 1, 0, 
                (struct sockaddr*)&src_addr, &addr_len);

        if (n > 0) {
            last_seen.store(std::chrono::steady_clock::now());
            unsigned char type = buffer[0];
            
			if (type == 0x01) {
				std::string payload(buffer + 1, n - 1);
				size_t delim = payload.find('\x1F');

				if (delim != std::string::npos) {
					std::string peer_nick = payload.substr(0, delim);
					std::string text = payload.substr(delim + 1);
					display_message(peer_nick, text, 1);
				} else {
					display_message("Friend", payload, 1);
				}
				continue; 
			} else if (type == 0x02) {
                if (peer_voice_on.load() && opus_decoder && pa_playback) {
					std::vector<unsigned char> audio_data((unsigned char*)buffer + 1, (unsigned char*)buffer + n);

					{
						std::lock_guard<std::mutex> lock(audio_queue_mutex);
						if (audio_queue.size() >= MAX_AUDIO_QUEUE_SIZE) {
							audio_queue.pop(); 
						}
						audio_queue.push(audio_data);
					}
					audio_queue_cv.notify_one();}
                continue;
            } else if (type == 0x03) {
				bool voice_state = (buffer[1] == 1);
				peer_voice_on.store(voice_state);
				if (!voice_state && pa_playback) {
					int err;
					pa_simple_flush(pa_playback, &err);
				}
				refresh_status_bar();
				continue;
			}
            buffer[n] = '\0';
            std::string msg(buffer);

            if (msg == "KEEPALIVE_PING") {
                sendto(global_sock, "KEEPALIVE_PONG", 14, 0, (struct sockaddr*)&src_addr, addr_len);
				continue;
			} else if (msg == "KEEPALIVE_PONG") {
				continue;
			} else if (msg.find("PUNCH:") == 0) {
                if (msg.substr(6) == SECRET_KEY) {
                    remote_peer_addr = src_addr;
                    if (!connected.load()) {
                        connected.store(true);
                        sendto(global_sock, "PUNCH_ACK", 9, 0, (struct sockaddr*)&src_addr, addr_len);
                        display_message("System", "Handshake received! Connected.", 3);
                    }
                }
				continue;
            } else if (msg == "PUNCH_ACK") {
                remote_peer_addr = src_addr;
                if (!connected.load()) {
                    connected.store(true);
                    display_message("System", "Handshake ACK! Connected.", 3);
                }
				continue;
			} else if (msg.find("DISCOVER:") == 0) {
				std::string peer_port = msg.substr(9);
				std::string peer_ip = inet_ntoa(src_addr.sin_addr);
				display_message("System", "Found peer on LAN! IP: " + peer_ip + ":" + peer_port, 3);
				// При желании можно тут же вызвать punch_and_connect(peer_ip, std::stoi(peer_port));
				continue;
			} else if (connected.load() && type > 0x1F) { 
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

		display_message("System", "Initializing audio engine...", 3);
		init_voice_rx();

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

		listening.store(true);
		std::thread(udp_listen_loop).detach();
		std::thread(check_timeout).detach();
		std::thread(heartbeat_loop).detach();
		
		active_upnp_port.store(port);
		std::thread(setup_upnp, port).detach();
		std::thread(voice_capture_thread).detach();
		std::thread(voice_playback_thread).detach();

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
	} else if (cmd == "/vc") {
		my_voice_on = !my_voice_on.load();

		if (!my_voice_on.load()) {
			if (pa_playback) {
				int err;
				pa_simple_flush(pa_playback, &err);
			}
		}

		unsigned char notify[2] = {0x03, (unsigned char)(my_voice_on ? 1 : 0)};
		if (connected.load()) {
			sendto(global_sock, notify, 2, 0, (struct sockaddr*)&remote_peer_addr, sizeof(remote_peer_addr));
		}

		display_message("System", my_voice_on ? "Voice chat ENABLED" : "Voice chat DISABLED", 3);
		refresh_status_bar();
	} else if (cmd == "/nick") {
		if (arg1.empty()) {
			display_message("System", "Usage: /nick <name>", 3);
			return;
		}
		my_nick = arg1;
		display_message("System", "Nickname changed to " + my_nick, 3);
	}
	else if (cmd == "/scan") {
		if (arg1.empty()) {
			display_message("System", "Usage: /scan <port>", 3);
			return;
		}
		int scan_port = std::stoi(arg1);
		int bsock = socket(AF_INET, SOCK_DGRAM, 0);

		int broadcastEnable = 1;
		setsockopt(bsock, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));

		struct sockaddr_in baddr;
		memset(&baddr, 0, sizeof(baddr));
		baddr.sin_family = AF_INET;
		baddr.sin_port = htons(scan_port);
		baddr.sin_addr.s_addr = inet_addr("255.255.255.255");

		std::string msg = "DISCOVER:" + std::to_string(active_upnp_port.load());
		sendto(bsock, msg.c_str(), msg.length(), 0, (struct sockaddr*)&baddr, sizeof(baddr));

		close(bsock);
		display_message("System", "Scanning LAN on port " + arg1 + "...", 3);
	}
	else {
		display_message("System", "Unknown command or not connected.", 3);
	}
}

void rebuild_ui(const std::string& current_input) {
    std::lock_guard<std::mutex> lock(ui_mutex);
    
    getmaxyx(stdscr, max_y, max_x);
    
    int input_content_len = current_input.length() + 2; 
    int input_height = (input_content_len / (max_x - 2)) + 3;
    if (input_height > 10) input_height = 10;

    if (chat_bg) { delwin(chat_text); delwin(chat_bg); }
    if (input_bg) { delwin(input_text); delwin(input_bg); }

    chat_bg = newwin(max_y - input_height, max_x, 0, 0);
    input_bg = newwin(input_height, max_x, max_y - input_height, 0);
    
    chat_text = derwin(chat_bg, max_y - input_height - 2, max_x - 2, 1, 1);
    input_text = derwin(input_bg, input_height - 2, max_x - 2, 1, 1);

    scrollok(chat_text, TRUE);
    box(chat_bg, 0, 0);
    box(input_bg, 0, 0);
    
    refresh();
    wnoutrefresh(chat_bg);
    wnoutrefresh(input_bg);
    doupdate();
}

void start_gui() {
	initscr();
	raw();
	noecho();
	start_color();
	init_pair(1, COLOR_RED, COLOR_BLACK);   
	init_pair(2, COLOR_MAGENTA, COLOR_BLACK); 
	init_pair(3, COLOR_CYAN, COLOR_BLACK);    

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

		if (ch == KEY_RESIZE) {
			rebuild_ui(current_input);
			continue;
		}

		if (ch != ERR) {
			bool input_changed = false;

			if (ch == '\n') {
				if (current_input == "/quit") {
					break;
				}
				else if (current_input.find("/") == 0) {
					handle_command(current_input);
				}
				else if (!current_input.empty()) {
					if (connected.load()) {
						std::string packet;
						packet += (char)0x01;
						packet += my_nick + "\x1F" + current_input;
						sendto(global_sock, packet.c_str(), packet.length(), 0, 
								(struct sockaddr*)&remote_peer_addr, sizeof(remote_peer_addr));
						display_message("You", current_input, 2);
					} else {
						display_message("System", "Not connected.", 3);
					}
				}

				current_input.clear();
				input_changed = true;
			}
			else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
				if (!current_input.empty()) {
					current_input.pop_back();
					input_changed = true;
				}
			} 
			else if (ch < 0x100 && isprint(ch)) {
				current_input += (char)ch;
				input_changed = true;
			}

			if (input_changed) {
				int needed_h = (current_input.length() / (max_x - 2)) + 3;
				if (needed_h != getmaxy(input_bg)) {
					rebuild_ui(current_input);
				}

				wclear(input_text);
				mvwprintw(input_text, 0, 0, "> %s", current_input.c_str());
				wrefresh(input_text);
			}
		}

		static auto last_ui_update = std::chrono::steady_clock::now();
		if (std::chrono::steady_clock::now() - last_ui_update > std::chrono::milliseconds(500)) {
			refresh_status_bar();
			last_ui_update = std::chrono::steady_clock::now();
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
    endwin();
}

int main() {
	start_gui();
	cleanup_upnp();
	if (pa_playback) pa_simple_free(pa_playback);
	if (global_sock != -1) close(global_sock);
	return 0;
}
