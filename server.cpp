// Операционные системы. Практика 2.
// TCP-сервер с безопасной обработкой SIGHUP через pselect().
// Linux / macOS / WSL2, C++17, один поток.

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// Эти флаги изменяются только обработчиком сигнала.
// В обработчике запрещено пользоваться cout, malloc и другими
// не async-signal-safe операциями.
volatile sig_atomic_t got_sighup = 0;
volatile sig_atomic_t stop_requested = 0;

void signal_handler(int signum) {
    if (signum == SIGHUP) {
        got_sighup = 1;
    } else if (signum == SIGINT || signum == SIGTERM) {
        stop_requested = 1;
    }
}

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

bool install_handler(int signum) {
    struct sigaction action {};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // Без SA_RESTART: pselect() может завершиться с EINTR.
    return sigaction(signum, &action, nullptr) == 0;
}

int main(int argc, char* argv[]) {
    int port = 8080;
    if (argc > 2) {
        std::cerr << "Использование: " << argv[0] << " [порт]\n";
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        char* end = nullptr;
        errno = 0;
        const long value = std::strtol(argv[1], &end, 10);
        if (errno != 0 || end == argv[1] || *end != '\0' || value < 1 || value > 65535) {
            std::cerr << "Неверный порт (допустимо 1..65535)\n";
            return EXIT_FAILURE;
        }
        port = static_cast<int>(value);
    }

    // Блокируем сигналы до регистрации обработчиков и проверки флагов.
    // В обычном коде они остаются заблокированными.
    sigset_t blocked_mask;
    sigset_t original_mask;
    sigemptyset(&blocked_mask);
    sigaddset(&blocked_mask, SIGHUP);
    sigaddset(&blocked_mask, SIGINT);
    sigaddset(&blocked_mask, SIGTERM);

    if (sigprocmask(SIG_BLOCK, &blocked_mask, &original_mask) == -1) {
        perror("sigprocmask");
        return EXIT_FAILURE;
    }

    if (!install_handler(SIGHUP) || !install_handler(SIGINT) ||
        !install_handler(SIGTERM)) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    // Во время pselect() сигналы будут временно разблокированы.
    // Маска и переход в ожидание применяются атомарно.
    sigset_t wait_mask = original_mask;
    sigdelset(&wait_mask, SIGHUP);
    sigdelset(&wait_mask, SIGINT);
    sigdelset(&wait_mask, SIGTERM);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    if (server_fd >= FD_SETSIZE) {
        std::cerr << "Серверный дескриптор слишком большой для fd_set\n";
        close(server_fd);
        return EXIT_FAILURE;
    }

    // Чтобы порт можно было быстро использовать после перезапуска.
    const int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        perror("setsockopt(SO_REUSEADDR)");
        close(server_fd);
        return EXIT_FAILURE;
    }

    // Неблокирующий режим: accept()/recv() не должны неожиданно
    // остановить основной цикл после того, как pselect() проснулся.
    if (!set_nonblocking(server_fd)) {
        perror("fcntl(server_fd)");
        close(server_fd);
        return EXIT_FAILURE;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }
    if (listen(server_fd, 16) == -1) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    std::cout << "Сервер слушает TCP-порт " << port << " (PID: " << getpid() << ")\n";
    std::cout << "Ожидаю подключения и SIGHUP. Для выхода нажмите Ctrl+C.\n";
    std::cout << std::flush;

    int client_fd = -1;  // Храним только одно соединение.
    int exit_status = EXIT_SUCCESS;

    while (true) {
        // Здесь SIGHUP/SIGINT/SIGTERM заблокированы, поэтому
        // чтение и сброс флагов не пересекаются с обработчиком.
        if (got_sighup) {
            got_sighup = 0;
            std::cout << "Получен сигнал SIGHUP\n";
        }
        if (stop_requested) {
            std::cout << "Получен сигнал завершения, останавливаю сервер\n";
            break;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        int max_fd = server_fd;

        if (client_fd != -1) {
            FD_SET(client_fd, &read_fds);
            max_fd = std::max(max_fd, client_fd);
        }

        // Главное: одна точка ожидания и атомарная смена маски сигналов.
        const int ready = pselect(max_fd + 1, &read_fds, nullptr, nullptr,
                                  nullptr, &wait_mask);

        if (ready == -1) {
            if (errno == EINTR) {
                continue;  // Сигнал обработан; на следующем витке прочитаем флаг.
            }
            perror("pselect");
            exit_status = EXIT_FAILURE;
            break;
        }

        if (FD_ISSET(server_fd, &read_fds)) {
            // Забираем все ожидающие подключения, не блокируя цикл.
            while (true) {
                int new_fd = accept(server_fd, nullptr, nullptr);
                if (new_fd == -1) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    perror("accept");
                    break;
                }

                std::cout << "Новое TCP-подключение (fd=" << new_fd << ")\n";

                if (client_fd != -1) {
                    std::cout << "Клиент уже есть: новое соединение закрыто\n";
                    close(new_fd);
                } else if (new_fd >= FD_SETSIZE) {
                    std::cerr << "Дескриптор вне диапазона fd_set: соединение закрыто\n";
                    close(new_fd);
                } else if (!set_nonblocking(new_fd)) {
                    perror("fcntl(client_fd)");
                    close(new_fd);
                } else {
                    client_fd = new_fd;
                    std::cout << "Соединение сохранено\n";
                }
            }
        }

        if (client_fd != -1 && FD_ISSET(client_fd, &read_fds)) {
            char buffer[4096];
            const ssize_t count = recv(client_fd, buffer, sizeof(buffer), 0);
            if (count > 0) {
                std::cout << "Получено байт: " << count << '\n';
            } else if (count == 0) {
                std::cout << "Клиент отключился\n";
                close(client_fd);
                client_fd = -1;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                perror("recv");
                close(client_fd);
                client_fd = -1;
            }
        }
        std::cout << std::flush;
    }

    if (client_fd != -1) close(client_fd);
    close(server_fd);
    if (sigprocmask(SIG_SETMASK, &original_mask, nullptr) == -1) {
        perror("sigprocmask(restore)");
        exit_status = EXIT_FAILURE;
    }
    std::cout << "Сервер завершён\n";
    return exit_status;
}
