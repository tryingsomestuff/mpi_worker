#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
constexpr std::int32_t kCommandWork = 1;
constexpr std::int32_t kCommandStop = 2;
constexpr const char* kEndpointFile = "mpi_port.txt";

struct WorkPayload
{
    std::int64_t iteration;
    std::int64_t begin;
    std::int64_t count;
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

bool read_endpoint(std::string* host, std::uint16_t* port)
{
    std::ifstream input(kEndpointFile);
    if(!input)
    {
        return false;
    }

    unsigned port_value = 0;
    input >> *host >> port_value;
    if(!input)
    {
        return false;
    }

    *port = static_cast<std::uint16_t>(port_value);
    return true;
}

int connect_to_master(const std::string& host, std::uint16_t port)
{
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(socket_fd < 0)
    {
        return -1;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if(inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
    {
        close(socket_fd);
        return -1;
    }

    if(connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

long long compute_chunk(long long begin, long long count, int iteration)
{
    long long partial = 0;
    const long long end = begin + count;
    for(long long value = begin; value < end; ++value)
    {
        partial += (value + 1) * (iteration + 1);
    }
    return partial;
}
}

int main()
{
    std::string host;
    std::uint16_t port = 0;
    if(!read_endpoint(&host, &port))
    {
        std::cerr << "[worker] endpoint introuvable dans " << kEndpointFile << std::endl;
        return 1;
    }

    const int socket_fd = connect_to_master(host, port);
    if(socket_fd < 0)
    {
        std::cerr << "[worker] connexion au maître impossible sur " << host << ':' << port << std::endl;
        return 1;
    }

    const int pid = static_cast<int>(getpid());
    std::cout << "[worker pid=" << pid << "] connecté" << std::endl;

    while(true)
    {
        std::int32_t command = 0;
        if(!recv_all(socket_fd, &command, sizeof(command)))
        {
            break;
        }

        if(command == kCommandStop)
        {
            std::cout << "[worker pid=" << pid << "] arrêt demandé" << std::endl;
            break;
        }

        if(command == kCommandWork)
        {
            WorkPayload payload = {};
            if(!recv_all(socket_fd, &payload, sizeof(payload)))
            {
                break;
            }

            const int iteration = static_cast<int>(payload.iteration);
            const long long begin = static_cast<long long>(payload.begin);
            const long long count = static_cast<long long>(payload.count);
            const long long partial = compute_chunk(begin, count, iteration);

            std::cout << "[worker pid=" << pid << "] itération " << iteration
                      << ", begin=" << begin
                      << ", count=" << count
                      << ", résultat=" << partial << std::endl;

            const std::int64_t result = static_cast<std::int64_t>(partial);
            if(!send_all(socket_fd, &result, sizeof(result)))
            {
                break;
            }
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    close(socket_fd);
    return 0;
}