#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace
{
constexpr std::int32_t kCommandWork = 1;
constexpr std::int32_t kCommandStop = 2;

constexpr const char* kAddRequestDir = "worker_requests";
constexpr const char* kRemoveRequestDir = "worker_kill_requests";
constexpr const char* kEndpointFile = "mpi_port.txt";
constexpr const char* kHost = "127.0.0.1";

namespace fs = std::filesystem;

struct WorkPayload
{
    std::int64_t iteration;
    std::int64_t begin;
    std::int64_t count;
};

struct WorkerHandle
{
    int id;
    int socket_fd;
};

bool send_all(int fd, const void* buffer, std::size_t size)
{
    const char* data = static_cast<const char*>(buffer);
    std::size_t sent = 0;
    while(sent < size)
    {
        const ssize_t rc = send(fd, data + sent, size - sent, 0);
        if(rc <= 0)
        {
            return false;
        }
        sent += static_cast<std::size_t>(rc);
    }
    return true;
}

bool recv_all(int fd, void* buffer, std::size_t size)
{
    char* data = static_cast<char*>(buffer);
    std::size_t received = 0;
    while(received < size)
    {
        const ssize_t rc = recv(fd, data + received, size - received, 0);
        if(rc <= 0)
        {
            return false;
        }
        received += static_cast<std::size_t>(rc);
    }
    return true;
}

long long compute_chunk_locally(long long begin, long long count, int iteration)
{
    long long partial = 0;
    const long long end = begin + count;
    for(long long value = begin; value < end; ++value)
    {
        partial += (value + 1) * (iteration + 1);
    }
    return partial;
}

std::vector<fs::path> consume_request_tickets(const fs::path& directory)
{
    std::vector<fs::path> tickets;

    if(!fs::exists(directory))
    {
        return tickets;
    }

    for(const fs::directory_entry& entry : fs::directory_iterator(directory))
    {
        if(entry.is_regular_file())
        {
            tickets.push_back(entry.path());
        }
    }

    std::sort(tickets.begin(), tickets.end());
    return tickets;
}

int create_server_socket(std::uint16_t* port)
{
    const int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd < 0)
    {
        return -1;
    }

    const int enable = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);

    if(bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        close(server_fd);
        return -1;
    }

    if(listen(server_fd, SOMAXCONN) != 0)
    {
        close(server_fd);
        return -1;
    }

    sockaddr_in bound_address = {};
    socklen_t bound_length = sizeof(bound_address);
    if(getsockname(server_fd, reinterpret_cast<sockaddr*>(&bound_address), &bound_length) != 0)
    {
        close(server_fd);
        return -1;
    }

    const int flags = fcntl(server_fd, F_GETFL, 0);
    if(flags >= 0)
    {
        fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
    }

    *port = ntohs(bound_address.sin_port);
    return server_fd;
}

void write_endpoint_file(std::uint16_t port)
{
    std::ofstream output(kEndpointFile, std::ios::trunc);
    output << kHost << ' ' << port << std::endl;
}

void initialize_request_directory()
{
    std::remove(kEndpointFile);
    fs::remove_all(kAddRequestDir);
    fs::remove_all(kRemoveRequestDir);
    fs::create_directory(kAddRequestDir);
    fs::create_directory(kRemoveRequestDir);
}

void cleanup_shared_files()
{
    std::remove(kEndpointFile);
    fs::remove_all(kAddRequestDir);
    fs::remove_all(kRemoveRequestDir);
}

void accept_pending_workers(int server_fd, std::vector<WorkerHandle>& workers, int* next_worker_id)
{
    while(true)
    {
        const int worker_fd = accept(server_fd, nullptr, nullptr);
        if(worker_fd < 0)
        {
            break;
        }

        workers.push_back({(*next_worker_id)++, worker_fd});
        std::cout << "[master] worker #" << workers.back().id
                  << " connecté, total actif=" << workers.size() << std::endl;
    }
}

void stop_one_worker(std::vector<WorkerHandle>& workers)
{
    if(workers.empty())
    {
        return;
    }

    WorkerHandle worker = workers.back();
    workers.pop_back();

    const std::int32_t command = kCommandStop;
    send_all(worker.socket_fd, &command, sizeof(command));
    close(worker.socket_fd);

    std::cout << "[master] worker #" << worker.id
              << " arrêté, total actif=" << workers.size() << std::endl;
}

void remove_worker_at(std::vector<WorkerHandle>& workers, std::size_t index)
{
    close(workers[index].socket_fd);
    workers.erase(workers.begin() + static_cast<std::ptrdiff_t>(index));
}
}

int main(int argc, char** argv)
{
    const int iteration_count = (argc > 1) ? std::max(1, std::stoi(argv[1])) : 8;
    const long long total_work_items = (argc > 2) ? std::max(1LL, std::stoll(argv[2])) : 120;
    const int pause_ms = (argc > 3) ? std::max(0, std::stoi(argv[3])) : 2000;

    initialize_request_directory();

    std::uint16_t port = 0;
    const int server_fd = create_server_socket(&port);
    if(server_fd < 0)
    {
        std::cerr << "[master] impossible d'ouvrir le serveur TCP" << std::endl;
        cleanup_shared_files();
        return 1;
    }

    write_endpoint_file(port);

    std::cout << "Endpoint: " << kHost << ':' << port << std::endl;
    std::cout << "Iterations: " << iteration_count
              << ", work items/iteration: " << total_work_items
              << ", pause(ms): " << pause_ms << std::endl;
    std::cout << "Le maître lira les demandes d'ajout dans " << kAddRequestDir
              << " et les demandes d'arrêt dans " << kRemoveRequestDir
              << " au début de chaque itération." << std::endl;

    std::vector<WorkerHandle> workers;
    int next_worker_id = 1;

    for(int iteration = 0; iteration < iteration_count; ++iteration)
    {
        accept_pending_workers(server_fd, workers, &next_worker_id);

        const std::vector<fs::path> removal_requests = consume_request_tickets(kRemoveRequestDir);
        if(!removal_requests.empty())
        {
            std::cout << "[master] " << removal_requests.size()
                      << " demande(s) d'arrêt en attente" << std::endl;
        }

        for(std::size_t index = 0; index < removal_requests.size(); ++index)
        {
            if(workers.empty())
            {
                std::cout << "[master] aucune ressource à arrêter pour la demande "
                          << (index + 1) << std::endl;
            }
            else
            {
                stop_one_worker(workers);
            }
            fs::remove(removal_requests[index]);
        }

        const std::vector<fs::path> requested_workers = consume_request_tickets(kAddRequestDir);
        if(!requested_workers.empty())
        {
            std::cout << "[master] " << requested_workers.size()
                      << " demande(s) de worker en attente" << std::endl;
        }
        for(const fs::path& ticket : requested_workers)
        {
            fs::remove(ticket);
        }

        accept_pending_workers(server_fd, workers, &next_worker_id);

        std::cout << "[master] itération " << iteration
                  << ", workers actifs=" << workers.size() << std::endl;

        long long global_sum = 0;
        if(workers.empty())
        {
            global_sum = compute_chunk_locally(0, total_work_items, iteration);
            std::cout << "[master] aucun worker, calcul local -> " << global_sum << std::endl;
        }
        else
        {
            const long long worker_count = static_cast<long long>(workers.size());
            const long long base_chunk = total_work_items / worker_count;
            const long long remainder = total_work_items % worker_count;

            long long begin = 0;
            for(std::size_t index = 0; index < workers.size(); ++index)
            {
                const long long chunk = base_chunk + (static_cast<long long>(index) < remainder ? 1 : 0);
                const std::int32_t command = kCommandWork;
                const WorkPayload payload = {
                    static_cast<std::int64_t>(iteration),
                    static_cast<std::int64_t>(begin),
                    static_cast<std::int64_t>(chunk)
                };

                if(!send_all(workers[index].socket_fd, &command, sizeof(command))
                   || !send_all(workers[index].socket_fd, &payload, sizeof(payload)))
                {
                    std::cout << "[master] perte du worker #" << workers[index].id << std::endl;
                    remove_worker_at(workers, index);
                    --index;
                    continue;
                }

                std::cout << "[master] tâche envoyée au worker #" << workers[index].id
                          << " : begin=" << begin
                          << ", count=" << chunk << std::endl;
                begin += chunk;
            }

            for(std::size_t index = 0; index < workers.size(); )
            {
                std::int64_t partial = 0;
                if(!recv_all(workers[index].socket_fd, &partial, sizeof(partial)))
                {
                    std::cout << "[master] résultat manquant, worker #" << workers[index].id
                              << " retiré" << std::endl;
                    remove_worker_at(workers, index);
                    continue;
                }

                global_sum += static_cast<long long>(partial);
                std::cout << "[master] résultat reçu de worker #" << workers[index].id
                          << " : " << partial << std::endl;
                ++index;
            }
        }

        std::cout << "[master] somme globale itération " << iteration
                  << " = " << global_sum << std::endl;

        if(pause_ms > 0 && iteration + 1 < iteration_count)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(pause_ms));
        }
    }

    while(!workers.empty())
    {
        stop_one_worker(workers);
    }

    close(server_fd);
    cleanup_shared_files();
    return 0;
}