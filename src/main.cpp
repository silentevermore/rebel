#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include <unistd.h>

#include <miniupnpc/miniupnpc.h>
#include <arpa/inet.h>
#include <sys/socket.h>

void receive_messages(int socket_fd) {
    char buffer[1024];
    while (true) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_received = read(socket_fd, buffer, sizeof(buffer) - 1);
        
        if (bytes_received <= 0) {
            std::cout << "\n[Соединение разорвано]" << std::endl;
            close(socket_fd);
            exit(0);
        }
        
        std::cout << "\033[91m\rДруг\033[0m: " << buffer << "\n\033[35m\rВы\033[0m: " << std::flush;
    }
}

void send_messages(int socket_fd) {
    std::string message;
    while (true) {
        std::cout << "\033[35m\rВы\033[0m: ";
        std::getline(std::cin, message);
        
        if (message == "/quit") {
            break;
        }
        
        if (!message.empty()) {
            send(socket_fd, message.c_str(), message.length(), 0);
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Использование:\n"
                  << "  Как сервер: " << argv[0] << " server <port>\n"
                  << "  Как клиент: " << argv[0] << " client <ip> <port>\n";
        return 1;
    }

    std::string mode = argv[1];
    int active_socket = -1;

    if (mode == "server") {
        if (argc != 4) {
            std::cerr << "Для сервера нужен IP и порт! (Например: ./chat server 10.0.0.1 8080)\n";
            return 1;
        }
        std::string ip = argv[2];
        int port = std::stoi(argv[3]);

        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_port = htons(port);

        if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) <= 0) {
            std::cerr << "Ошибка: Неверный формат IP-адреса!" << std::endl;
            return 1;
        }

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            std::cerr << "Ошибка bind! Возможно, этот IP вам не принадлежит или порт занят." << std::endl;
            return 1;
        }
        
        listen(server_fd, 1);

        struct sockaddr_in bound_addr;
        socklen_t len = sizeof(bound_addr);
        
        if (getsockname(server_fd, (struct sockaddr *)&bound_addr, &len) == 0) {
            std::cout << "[Сервер] Успешно запущен!\n"
                      << "Привязан к IP: " << inet_ntoa(bound_addr.sin_addr) << "\n"
                      << "Слушаем порт: " << ntohs(bound_addr.sin_port) << "\n"
                      << "Ожидание подключения..." << std::endl;
        }
        
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        active_socket = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        
        std::cout << "[Сервер] Друг подключился! Его IP: " << inet_ntoa(client_addr.sin_addr) << std::endl;
        close(server_fd);
    } else if (mode == "client") {
        if (argc != 4) {
            std::cerr << "Для клиента нужен IP и порт!\n";
            return 1;
        }
        std::string ip = argv[2];
        int port = std::stoi(argv[3]);

        active_socket = socket(AF_INET, SOCK_STREAM, 0);
        
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr);

        std::cout << "Подключаюсь к " << ip << ":" << port << "..." << std::endl;
        
        if (connect(active_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            std::cerr << "Ошибка подключения!" << std::endl;
            return 1;
        }
        std::cout << "Подключено! Можно писать." << std::endl;
    } else {
        std::cerr << "Неизвестный режим. Используйте server или client." << std::endl;
        return 1;
    }

    std::thread receiver_thread(receive_messages, active_socket);
    receiver_thread.detach(); 

    send_messages(active_socket);

    close(active_socket);
    return 0;
}
