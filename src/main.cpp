#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include <unistd.h>
#include <atomic>
#include <fstream>

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <ncurses.h>
#include <mutex>

const std::string SECRET_KEY = "my_super_secret_p2p_key_123";
const std::string HISTORY_FILE = "chat_history.log";
std::string EXT_IP;

WINDOW *chat_bg, *chat_text, *input_bg, *input_text;
std::mutex ui_mutex;
std::atomic<int> active_socket{-1};

// ДОБАВЛЕНО: параметр save_to_file (по умолчанию true), чтобы не дублировать историю при загрузке
void display_message(const std::string& user, const std::string& msg, int color_pair, bool save_to_file = true) {
    std::lock_guard<std::mutex> lock(ui_mutex);
    wattron(chat_text, COLOR_PAIR(color_pair));
    wprintw(chat_text, "%s: ", user.c_str());
    wattroff(chat_text, COLOR_PAIR(color_pair));
    wprintw(chat_text, "%s\n", msg.c_str());
    
    wrefresh(chat_text);
    wrefresh(input_text);

    // ДОБАВЛЕНО: Сохраняем сообщение в файл
    if (save_to_file) {
        std::ofstream file(HISTORY_FILE, std::ios::app); // Открываем в режиме дозаписи (append)
        if (file.is_open()) {
            // Используем табуляцию (\t) как разделитель, чтобы потом легко распарсить строку
            file << user << "\t" << msg << "\n";
        }
    }
}

// ДОБАВЛЕНО: Функция для загрузки истории из файла
void load_history() {
    std::ifstream file(HISTORY_FILE);
    if (!file.is_open()) return; // Если файла еще нет (первый запуск), просто выходим

    std::string line;
    while (std::getline(file, line)) {
        size_t pos = line.find('\t');
        if (pos != std::string::npos) {
            std::string user = line.substr(0, pos);
            std::string msg = line.substr(pos + 1);

            // Восстанавливаем оригинальные цвета
            int color = 1;
            if (user == "You") color = 2;
            else if (user == "System") color = 3;

            // Выводим на экран, но передаем false, чтобы не записать это в файл по второму кругу
            display_message(user, msg, color, false);
        }
    }
}

#include <netdb.h>

std::string get_ip_via_stun() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "";

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(19302); // Стандартный порт STUN
    
    struct hostent *server = gethostbyname("stun.l.google.com");
    if (!server) return "";
    memcpy(&servaddr.sin_addr.s_addr, server->h_addr, server->h_length);

    // Минимальный заголовок STUN Binding Request (20 байт)
    unsigned char stun_request[] = {
        0x00, 0x01, 0x00, 0x00, // Binding Request
        0x21, 0x12, 0xA4, 0x42, // Magic Cookie
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c // Transaction ID
    };

    sendto(sock, stun_request, sizeof(stun_request), 0, (struct sockaddr *)&servaddr, sizeof(servaddr));

    unsigned char buffer[512];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    
    // Ждем ответ (в реальном коде лучше добавить timeout через setsockopt)
    int n = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&from, &fromlen);
    close(sock);

    if (n > 20) {
        // Очень упрощенный парсинг атрибута MAPPED-ADDRESS или XOR-MAPPED-ADDRESS
        // В продакшене лучше использовать полноценную либу типа libnice или stun-c
        for (int i = 20; i < n - 4; i++) {
            if (buffer[i] == 0x00 && (buffer[i+1] == 0x01 || buffer[i+1] == 0x20)) {
                struct in_addr out_addr;
                memcpy(&out_addr, &buffer[i+8], 4);
                return inet_ntoa(out_addr);
            }
        }
    }
    return "";
}

std::string setup_upnp(int port) {
    int error = 0;
    // Увеличим время ожидания до 2000мс для стабильности
    struct UPNPDev * devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
    if (!devlist) return "";

    struct UPNPUrls urls;
    struct IGDdatas data;
    char lanaddr[64] = {0};
    char wanaddr[64] = {0}; // Тут роутер должен вернуть свой WAN IP
    
    // status == 1: найден валидный подключенный IGD
    // status == 2: найден валидный, но не подключенный
    int status = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));
    
    std::string external_ip = "";
    if (status == 1 || status == 2) {
        std::string port_str = std::to_string(port);
        // Пробрасываем порт
        int r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                    port_str.c_str(), port_str.c_str(), lanaddr, 
                                    "P2P Messenger", "TCP", nullptr, "0");
        
        if (r == 0) {
            // Если wanaddr не пустой и не похож на локальный — используем его
            if (strlen(wanaddr) > 0 && wanaddr[0] != '0') {
                external_ip = wanaddr;
            } else {
                // Запасной вариант, если wanaddr пустой
                char fallbackIP[40] = {0};
                if (UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, fallbackIP) == 0) {
                    external_ip = fallbackIP;
                }
            }
        }
        FreeUPNPUrls(&urls);
    }
    freeUPNPDevlist(devlist);
    return external_ip;
}

void server_network_loop(int server_fd) {
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock < 0) continue;

        char handshake_buffer[1024] = {0};
        recv(client_sock, handshake_buffer, sizeof(handshake_buffer), 0);

        if (std::string(handshake_buffer) != SECRET_KEY) {
            close(client_sock);
            continue; 
        }

        display_message("System", "Friend joined!", 3);
        active_socket.store(client_sock);

        char buffer[1024];
        while (true) {
            memset(buffer, 0, sizeof(buffer));
            ssize_t bytes_received = read(client_sock, buffer, sizeof(buffer) - 1);

            if (bytes_received <= 0) {
                display_message("System", "[ Connection closed. Waiting for new connection... ]", 3);
                close(client_sock);
                active_socket.store(-1);
                break; 
            }
            display_message("Friend", buffer, 1);
        }
    }
}

void client_network_loop() {
    char buffer[1024];
    while (true) {
        int sock = active_socket.load();
        if (sock == -1) break;

        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_received = read(sock, buffer, sizeof(buffer) - 1);

        if (bytes_received <= 0) {
            display_message("System", "[ Server disconnected ]", 3);
            close(sock);
            active_socket.store(-1);
            break;
        }
        display_message("Server/Friend", buffer, 1);
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
    
    box(chat_bg, 0, 0);
    box(input_bg, 0, 0);
    wrefresh(chat_bg);
    wrefresh(input_bg);

    // ДОБАВЛЕНО: Загружаем историю перед тем, как начнется общение
    load_history();

	display_message("System", "External IP: " + EXT_IP, 3);

    char input_buf[256];
    while (true) {
        memset(input_buf, 0, 256);
        
        wclear(input_text);
        wprintw(input_text, "You: ");
        wrefresh(input_text);
        
        echo();
        wgetnstr(input_text, input_buf, 255);
        noecho();

        if (std::string(input_buf) == "/quit") break;

        if (strlen(input_buf) > 0) {
            int sock = active_socket.load();
            if (sock != -1) {
                send(sock, input_buf, strlen(input_buf), 0);
                display_message("You", input_buf, 2);
            } else {
                display_message("System", "[ No active connection to send message ]", 3);
            }
        }
    }
    endwin();
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage:\n"
                  << "  As server: " << argv[0] << " server <port>\n"
                  << "  As client: " << argv[0] << " client <ip> <port>\n";
        return 1;
    }
    
    std::string mode = argv[1];

    if (mode == "server") {
        // ИСПРАВЛЕНО: argc должно быть 3 для сервера (app, server, port), а port лежит в argv[2]
        if (argc != 3) {
            std::cerr << "Invalid arguments for server\n";
            return 1;
        }
        int port = std::stoi(argv[2]); 
        
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = INADDR_ANY;

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) return 1;
        listen(server_fd, 5);

        std::cout << "Configuring UPnP..." << std::endl;

		std::string upnp_ip = setup_upnp(port);
		std::string stun_ip = get_ip_via_stun();

		if (upnp_ip.empty() || upnp_ip.find("192.168.") == 0 || upnp_ip.find("10.") == 0) {
			// UPnP выдал локальный IP или ошибку — доверяем STUN
			EXT_IP = stun_ip;
		} else {
			EXT_IP = upnp_ip;
		}

        std::cout << "Waiting for connections. Press Enter to open GUI..." << std::endl;
        
        std::thread net_thread(server_network_loop, server_fd);
        net_thread.detach();
    } else if (mode == "client") {
        if (argc != 4) return 1;
        std::string ip = argv[2];
        int port = std::stoi(argv[3]);

        int client_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr);

        if (connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            std::cerr << "Connection failed" << std::endl;
            return 1;
        }
        send(client_fd, SECRET_KEY.c_str(), SECRET_KEY.length(), 0);
        
        active_socket.store(client_fd);
        
        std::thread net_thread(client_network_loop);
        net_thread.detach();
    }

    start_gui();

    int sock = active_socket.load();
    if (sock != -1) close(sock);
    
    return 0;
}
