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
#include "include/json.hpp"
#include <arpa/inet.h>

using json = nlohmann::json;

// Data Structures
struct PeerInfo
{
    int peer_id;
    int port;
    std::string ip;
    std::set<int> chunk_ids;
};

struct ChunkInfo
{
    int chunk_id;
    std::set<int> peers_having_chunk;
    std::string data; // Data of the chunk 
};

struct FileInfo
{
    std::string file_hash;
    std::string file_name;
    int total_chunks;
    std::unordered_map<int, ChunkInfo> chunks;
    std::unordered_map<int, PeerInfo> peer_list;
};

// Global file table
std::unordered_map<std::string, FileInfo> file_table;
std::mutex file_table_mutex;

void printFileTable()
{
    for (const auto &[file_hash, file_info] : file_table)
    {
        std::cout << "File: " << file_hash << "\n";
        std::cout << "  Name: " << file_info.file_name << "\n";
        std::cout << "  Total Chunks: " << file_info.total_chunks << "\n";

        std::cout << "  Chunks:\n";
        for (const auto &[chunk_id, chunk_info] : file_info.chunks)
        {
            std::cout << "    Chunk ID: " << chunk_id << "\n";
            std::cout << "      Data: " << chunk_info.data << "\n";
            std::cout << "      Peers having this chunk: ";
            for (int peer : chunk_info.peers_having_chunk)
            {
                std::cout << peer << " ";
            }
            std::cout << "\n";
        }

        std::cout << "  Peers:\n";
        for (const auto &[peer_id, peer_info] : file_info.peer_list)
        {
            std::cout << "    Peer ID: " << peer_id << "\n";
            std::cout << "    Peer port: " << peer_info.port << "\n";
            std::cout << "    Peer ip: " << peer_info.ip << "\n";
            std::cout << "      Chunks owned by this peer: ";
            for (int chunk_id : peer_info.chunk_ids)
            {
                std::cout << chunk_id << " ";
            }
            std::cout << "\n";
        }
        std::cout << "--------------------------------\n";
    }
}

class CentralizedServer
{
private:
    int server_fd;
    int port;

public:
    CentralizedServer(int port) : port(port) {}

    bool start();
    void handle_client(int client_socket, const std::string &client_ip, int client_port);
    void register_chunk(const std::string &file_hash, const std::string &file_name, int chunk_id, int peer_id, int port, std::string ip);
    std::vector<int> get_chunk_locations(const std::string &file_hash, int chunk_id);
    std::vector<std::pair<std::string, std::string>> list_files();
    void send_chunk_data(int client_socket, const std::string &file_hash, int chunk_id);
    void register_peer(const std::string &file_hash, int peer_id, int port, const std::string &ip);
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

    struct sockaddr_in client_addr;
    socklen_t client_len = 0;

    while (true)
    {

        int client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0)
        {
            perror("Accept failed");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        if (getpeername(client_socket, (struct sockaddr *)&client_addr, &client_len) == 0)
        {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(client_addr.sin_addr), ip_str, INET_ADDRSTRLEN); // Convert to readable format

            int client_port = ntohs(client_addr.sin_port);
            std::cout << "Connection accepted: IP = " << ip_str << ", Port = " << client_port << std::endl;
            std::thread(&CentralizedServer::handle_client, this, client_socket, std::string(ip_str), client_port).detach();
        }
        else
        {
            std::cerr << "Failed to get peer name: " << strerror(errno) << std::endl;
        }
    }

    return true;
}

void CentralizedServer::handle_client(int client_socket, const std::string &client_ip, int client_port)
{
    char buffer[1024] = {0};
    int bytes_read;

    while ((bytes_read = read(client_socket, buffer, 1024)) > 0)
    {

        std::string request(buffer, bytes_read);
        json request_json;
        std::cout << request << std::endl;
        try
        {
            // Parse JSON request
            request_json = json::parse(request);
        }
        catch (json::parse_error &e)
        {
            std::string response = "Invalid JSON format\n";
            send(client_socket, response.c_str(), response.length(), 0);
            continue;
        }

        std::string command = request_json["command"];

        if (command == "REGISTER")
        {
            std::string file_hash = request_json["file_hash"];
            std::string file_name = request_json["file_name"];
            int peer_id = request_json["peer_id"];
            int peer_port = request_json["port"];

            for (int chunk_id : request_json["chunks"])
            {
                register_chunk(file_hash, file_name, chunk_id, peer_id, peer_port, client_ip);
                std::cout << "Registered chunk " << chunk_id << " for file " << file_name << std::endl;
            }

            std::string response = "Chunks registered\n";
            send(client_socket, response.c_str(), response.length(), 0);
        }
        else if (command == "LOCATE")
        {
            std::string file_hash = request_json["file_hash"];
            int chunk_id = request_json["chunk_id"];
            auto locations = get_chunk_locations(file_hash, chunk_id);

            json response;
            response["status"] = "success";
            response["chunk_locations"] = locations;

            std::string response_str = response.dump();
            send(client_socket, response_str.c_str(), response_str.length(), 0);
        }
        else if (command == "LIST_FILES")
        {
            std::vector<std::pair<std::string, std::string>> map = list_files();

            json response;
            response["status"] = "success";
            response["files"] = json::array(); // Initialize as an array

            for (const auto &[file_hash, file_name] : map)
            {
                json file_info;
                file_info["file_hash"] = file_hash;
                file_info["file_name"] = file_name;
                response["files"].push_back(file_info);
            }

            std::string response_str = response.dump();
            send(client_socket, response_str.c_str(), response_str.length(), 0);
        }
        else if (command == "REQUEST_FILE")
        {
            std::string file_hash = request_json["file_hash"];

            json response;
            response["status"] = "success";

            response["chunks"] = json::array();

            {
                std::lock_guard<std::mutex> lock(file_table_mutex);
                if (file_table.find(file_hash) != file_table.end())
                {
                    response["total_chunks"] = file_table[file_hash].total_chunks;
                    // Loop through each chunk in the file
                    for (const auto &[chunk_id, chunk_info] : file_table[file_hash].chunks)
                    {
                        json chunk_data;
                        chunk_data["chunk_id"] = chunk_id;
                        chunk_data["peers"] = json::array();

                        // Add each peer's IP and port who has this chunk
                        for (int peer_id : chunk_info.peers_having_chunk)
                        {
                            if (file_table[file_hash].peer_list.find(peer_id) != file_table[file_hash].peer_list.end())
                            {
                                const PeerInfo &peer_info = file_table[file_hash].peer_list.at(peer_id);
                                json peer_data;
                                peer_data["peer_id"] = peer_id;
                                peer_data["ip"] = peer_info.ip;
                                peer_data["port"] = peer_info.port;
                                chunk_data["peers"].push_back(peer_data);
                            }
                        }

                        response["chunks"].push_back(chunk_data);
                    }
                }
                else
                {
                    response["status"] = "error";
                    response["message"] = "File not found";
                }
            }

            std::string response_str = response.dump();
            std::cout << response_str << std::endl;
            send(client_socket, response_str.c_str(), response_str.length(), 0);
        }
        else if (command == "CHECK_CONNECTION")
        {
            std::cout << "checking for connection\n";
        }
        else if (command == "GET_FILE_NAME")
        {
            std::string file_hash = request_json["file_hash"];

            json response;
            {
                std::lock_guard<std::mutex> lock(file_table_mutex);
                if (file_table.find(file_hash) != file_table.end())
                {
                    response["status"] = "success";
                    response["file_name"] = file_table[file_hash].file_name;
                }
                else
                {
                    response["status"] = "error";
                    response["message"] = "File not found";
                }
            }

            std::string response_str = response.dump();
            send(client_socket, response_str.c_str(), response_str.length(), 0);
        }
        else if (command == "REGISTER_PEER")
        {
            std::string file_hash = request_json["file_hash"];
            int peer_id = request_json["peer_id"];
            int peer_port = request_json["port"];

            // Register the peer without processing chunks
            register_peer(file_hash, peer_id, peer_port, client_ip);

            std::string response = "Peer registered successfully\n";
            send(client_socket, response.c_str(), response.length(), 0);
        }
        else
        {
            json response;
            response["status"] = "error";
            response["message"] = "Unknown command";

            std::string response_str = response.dump();
            send(client_socket, response_str.c_str(), response_str.length(), 0);
        }
    }
    close(client_socket);
    printFileTable();
}

void CentralizedServer::register_chunk(const std::string &file_hash, const std::string &file_name, int chunk_id, int peer_id, int port, std::string ip)
{
    std::lock_guard<std::mutex> lock(file_table_mutex);

    if (file_table.find(file_hash) == file_table.end())
    {
        file_table[file_hash] = FileInfo{file_hash, file_name, 0};
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
        file_info.peer_list[peer_id] = PeerInfo{peer_id, port, ip};
    }
    file_info.peer_list[peer_id].chunk_ids.insert(chunk_id);

    file_info.total_chunks = std::max(file_info.total_chunks, chunk_id + 1);

    std::cout << "Registered chunk " << chunk_id << " for file " << file_hash << " from peer " << peer_id << std::endl;
    printFileTable();
}

void CentralizedServer::register_peer(const std::string &file_hash, int peer_id, int port, const std::string &ip)
{
    std::lock_guard<std::mutex> lock(file_table_mutex);

    // Check if the file exists in the file table
    if (file_table.find(file_hash) == file_table.end())
    {
        std::cout << "File not found for hash: " << file_hash << std::endl;
        return;
    }

    FileInfo &file_info = file_table[file_hash];

    // Update peer info
    if (file_info.peer_list.find(peer_id) == file_info.peer_list.end())
    {
        file_info.peer_list[peer_id] = PeerInfo{peer_id, port, ip};
        std::cout << "Registered peer " << peer_id << " for file " << file_hash << std::endl;
    }
    else
    {
        // Peer already exists, update details if necessary
        PeerInfo &peer_info = file_info.peer_list[peer_id];
        peer_info.ip = ip;
        peer_info.port = port;
        std::cout << "Updated peer " << peer_id << " details for file " << file_hash << std::endl;
    }

    printFileTable();
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

std::vector<std::pair<std::string, std::string>> CentralizedServer::list_files()
{
    std::lock_guard<std::mutex> lock(file_table_mutex);
    std::vector<std::pair<std::string, std::string>> files; // Vector of pairs for (file_hash, file_name)

    for (const auto &entry : file_table)
    {
        const std::string &file_hash = entry.first;
        const std::string &file_name = entry.second.file_name;
        files.emplace_back(file_hash, file_name);
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