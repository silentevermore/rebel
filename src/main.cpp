#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include <unistd.h>
#include <atomic> // Добавлено для безопасной работы с сокетом между потоками

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <ncurses.h>
#include <mutex>

const std::string SECRET_KEY = "my_super_secret_p2p_key_123";
std::string EXT_IP;

// Разделяем окна на фоновые (для рамок) и текстовые (для контента)
WINDOW *chat_bg, *chat_text, *input_bg, *input_text;
std::mutex ui_mutex;
std::atomic<int> active_socket{-1}; // Глобальный сокет текущего чата

void display_message(const std::string& user, const std::string& msg, int color_pair) {
    std::lock_guard<std::mutex> lock(ui_mutex);
    wattron(chat_text, COLOR_PAIR(color_pair));
    wprintw(chat_text, "%s: ", user.c_str());
    wattroff(chat_text, COLOR_PAIR(color_pair));
    wprintw(chat_text, "%s\n", msg.c_str());
    
    // Обновляем только текстовые области, рамки остаются нетронутыми
    wrefresh(chat_text);
    wrefresh(input_text);
}

std::string setup_upnp(int port) {
    int error = 0;
    struct UPNPDev * devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
    struct UPNPUrls urls;
    memset(&urls, 0, sizeof(struct UPNPUrls));
    struct IGDdatas data;
    memset(&data, 0, sizeof(struct IGDdatas));
    
    char lanaddr[64] = {0};
    char wanaddr[64] = {0};
    std::string external_ip = "";

    if (devlist) {
        int status = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));
        if (status > 0) {
            std::string port_str = std::to_string(port);
            int r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                        port_str.c_str(), port_str.c_str(), lanaddr, 
                                        "P2P Messenger", "TCP", nullptr, "0");
            
            if (r == 0) {
                char externalIP[40] = {0};
                if (UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, externalIP) == 0) {
                    external_ip = externalIP;
                }
            }
            FreeUPNPUrls(&urls);
        }
        freeUPNPDevlist(devlist);
    }
    return external_ip;
}

// Поток сервера: бесконечно слушает входящие подключения
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
            continue; // Игнорируем чужаков и продолжаем слушать
        }

        display_message("System", "Friend joined!", 3);
        active_socket.store(client_sock);

        // Цикл общения с конкретным клиентом
        char buffer[1024];
        while (true) {
            memset(buffer, 0, sizeof(buffer));
            ssize_t bytes_received = read(client_sock, buffer, sizeof(buffer) - 1);

            if (bytes_received <= 0) {
                display_message("System", "[ Connection closed. Waiting for new connection... ]", 3);
                close(client_sock);
                active_socket.store(-1);
                break; // Выходим из цикла чтения, чтобы сервер вернулся к accept()
            }
            display_message("Friend", buffer, 1);
        }
    }
}

// Поток клиента: просто читает сообщения от сервера
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

    // Создаем фоновые окна для рамок
    chat_bg = newwin(max_y - 3, max_x, 0, 0);
    input_bg = newwin(3, max_x, max_y - 3, 0);
    
    // Создаем дочерние окна с отступом 1 символ для текста
    chat_text = derwin(chat_bg, max_y - 5, max_x - 2, 1, 1);
    input_text = derwin(input_bg, 1, max_x - 2, 1, 1);
    
    scrollok(chat_text, TRUE);
    
    // Рисуем рамки только один раз на фоне
    box(chat_bg, 0, 0);
    box(input_bg, 0, 0);
    wrefresh(chat_bg);
    wrefresh(input_bg);

	if (!EXT_IP.empty()) {
		display_message("System", "External IP: " + EXT_IP, 3);
	}

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
        if (argc != 4) return 1;
        int port = std::stoi(argv[3]);
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = INADDR_ANY;

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) return 1;
        listen(server_fd, 5); // Разрешаем очередь подключений

        std::cout << "Configuring UPnP..." << std::endl;
        EXT_IP = setup_upnp(port);
        std::cout << "Waiting for connections. Press Enter to open GUI..." << std::endl;
        
        // Запускаем сетевой цикл сервера в отдельном потоке
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
        
        // Запускаем цикл чтения клиента в отдельном потоке
        std::thread net_thread(client_network_loop);
        net_thread.detach();
    }

    // Вне зависимости от режима запускаем интерфейс в главном потоке
    start_gui();

    int sock = active_socket.load();
    if (sock != -1) close(sock);
    
    return 0;
}
