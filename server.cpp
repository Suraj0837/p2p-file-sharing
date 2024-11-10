#include <iostream>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <set>
#include <string>
#include <sstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <filesystem>

// Data Structures
struct PeerInfo
{
    int peer_id;
    std::set<int> chunk_ids;
};

struct ChunkInfo
{
    int chunk_id;
    std::set<int> peers_having_chunk;
    std::string data; // Data of the chunk (for simulating the server sending chunks) - dont use this to store the data in server, instead send directly to client
};

struct FileInfo
{
    std::string file_hash;
    int total_chunks;
    std::unordered_map<int, ChunkInfo> chunks;
    std::unordered_map<int, PeerInfo> peer_list;
};

// Global file table
std::unordered_map<std::string, FileInfo> file_table;
std::mutex file_table_mutex;

class CentralizedServer
{
private:
    int server_fd;
    int port;

public:
    CentralizedServer(int port) : port(port) {}

    bool start();
    void handle_client(int client_socket);
    void register_chunk(const std::string &file_hash, int chunk_id, int peer_id);
    std::vector<int> get_chunk_locations(const std::string &file_hash, int chunk_id);
    std::vector<std::string> list_files();
    void send_chunk_data(int client_socket, const std::string &file_hash, int chunk_id);
};

bool CentralizedServer::start()
{
    struct sockaddr_in address;
    int opt = 1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == 0)
    {
        perror("Socket failed");
        return false;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
    {
        perror("Setsockopt failed");
        close(server_fd);
        return false;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("Bind failed");
        close(server_fd);
        return false;
    }

    if (listen(server_fd, 5) < 0)
    {
        perror("Listen failed");
        close(server_fd);
        return false;
    }

    std::cout << "Server started on port " << port << std::endl;

    while (true)
    {
        int client_socket = accept(server_fd, NULL, NULL);
        if (client_socket < 0)
        {
            perror("Accept failed");
            continue;
        }
        std::thread(&CentralizedServer::handle_client, this, client_socket).detach();
    }

    return true;
}

void CentralizedServer::handle_client(int client_socket)
{
    char buffer[1024] = {0};
    int bytes_read;

    while ((bytes_read = read(client_socket, buffer, 1024)) > 0)
    {
        std::string request(buffer, bytes_read);
        std::istringstream iss(request);
        std::string command;
        iss >> command;

        if (command == "REGISTER")
        {
            std::string file_hash;
            int peer_id;
            iss >> file_hash >> peer_id;

            int chunk_id;
            while (iss >> chunk_id)
            {
                register_chunk(file_hash, chunk_id, peer_id);
            }

            std::string response = "Chunks registered\n";
            send(client_socket, response.c_str(), response.length(), 0);
        }
        else if (command == "LOCATE")
        {
            std::string file_hash;
            int chunk_id;
            iss >> file_hash >> chunk_id;
            auto locations = get_chunk_locations(file_hash, chunk_id);
            std::string response = "Chunk locations: ";
            for (int peer : locations)
            {
                response += std::to_string(peer) + " ";
            }
            response += "\n";
            send(client_socket, response.c_str(), response.length(), 0);
        }
        else if (command == "LIST_FILES")
        {
            std::vector<std::string> files = list_files();
            std::string response = "Available files: ";
            for (const std::string &file : files)
            {
                response += file + " ";
            }
            response += "\n";
            send(client_socket, response.c_str(), response.length(), 0);
        }
        else if (command == "REQUEST_FILE")
        {
            std::string file_hash;
            iss >> file_hash;
            int total_chunks = file_table[file_hash].total_chunks;
            for (int i = 0; i < total_chunks; ++i)
            {
                send_chunk_data(client_socket, file_hash, i);
            }
            std::string response = "File chunks sent\n";
            send(client_socket, response.c_str(), response.length(), 0);
        }
        else
        {
            std::string response = "Unknown command\n";
            send(client_socket, response.c_str(), response.length(), 0);
        }
    }
    close(client_socket);
}

void CentralizedServer::register_chunk(const std::string &file_hash, int chunk_id, int peer_id)
{
    std::lock_guard<std::mutex> lock(file_table_mutex);

    if (file_table.find(file_hash) == file_table.end())
    {
        file_table[file_hash] = FileInfo{file_hash, 0};
    }
    FileInfo &file_info = file_table[file_hash];

    // Update chunk info
    if (file_info.chunks.find(chunk_id) == file_info.chunks.end())
    {
        file_info.chunks[chunk_id] = ChunkInfo{chunk_id};
    }
    file_info.chunks[chunk_id].peers_having_chunk.insert(peer_id);

    // Update peer info
    if (file_info.peer_list.find(peer_id) == file_info.peer_list.end())
    {
        file_info.peer_list[peer_id] = PeerInfo{peer_id};
    }
    file_info.peer_list[peer_id].chunk_ids.insert(chunk_id);

    file_info.total_chunks = std::max(file_info.total_chunks, chunk_id + 1);

    std::cout << "Registered chunk " << chunk_id << " for file " << file_hash << " from peer " << peer_id << std::endl;
}

std::vector<int> CentralizedServer::get_chunk_locations(const std::string &file_hash, int chunk_id)
{
    std::lock_guard<std::mutex> lock(file_table_mutex);
    std::vector<int> locations;

    if (file_table.find(file_hash) != file_table.end() && file_table[file_hash].chunks.find(chunk_id) != file_table[file_hash].chunks.end())
    {
        for (int peer_id : file_table[file_hash].chunks[chunk_id].peers_having_chunk)
        {
            locations.push_back(peer_id);
        }
    }

    return locations;
}

std::vector<std::string> CentralizedServer::list_files()
{
    std::lock_guard<std::mutex> lock(file_table_mutex);
    std::vector<std::string> files;
    for (const auto &entry : file_table)
    {
        files.push_back(entry.first);
    }
    return files;
}

void CentralizedServer::send_chunk_data(int client_socket, const std::string &file_hash, int chunk_id)
{
    std::lock_guard<std::mutex> lock(file_table_mutex);

    if (file_table.find(file_hash) != file_table.end() && file_table[file_hash].chunks.find(chunk_id) != file_table[file_hash].chunks.end())
    {
        const ChunkInfo &chunk_info = file_table[file_hash].chunks[chunk_id];
        std::string chunk_data = chunk_info.data; // Simulating the chunk data

        // Send chunk data to the client
        send(client_socket, chunk_data.c_str(), chunk_data.size(), 0);
        std::cout << "Sent chunk " << chunk_id << " of file " << file_hash << "\n";
    }
    else
    {
        std::string error_response = "Chunk not found\n";
        send(client_socket, error_response.c_str(), error_response.length(), 0);
    }
}

int main()
{
    int port = 8080;
    CentralizedServer server(port);
    if (!server.start())
    {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }
    return 0;
}
