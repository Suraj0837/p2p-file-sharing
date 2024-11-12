#include <sstream>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <set>
#include <unordered_map>
#include <mutex>
#include <arpa/inet.h>
#include <unistd.h>
#include <filesystem>
#include "utilities.h"

#define CENTRAL_SERVER_PORT 8080

// Data Structures
struct ChunkInfo
{
    int chunk_id;
    std::string data;
};

class PeerClient
{
private:
    int sock;
    int peer_id;
    struct sockaddr_in server_address;
    std::string server_ip = "127.0.0.1";
    int server_port;

public:
    PeerClient(const std::string &ip, int port, int id)
        : server_ip(ip), server_port(port), peer_id(id)
    {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            perror("Socket creation error");
            exit(EXIT_FAILURE);
        }
        server_address.sin_family = AF_INET;
        server_address.sin_port = htons(CENTRAL_SERVER_PORT);
        inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr);
    }

    bool connect_to_server();
    void register_file(const std::string &filepath);
    std::vector<std::string> get_available_files();
    void download_file(const std::string &file_hash, int total_chunks);
    void receive_chunk(const std::string &file_hash, int chunk_id);
    void handle_disconnection();
    // destructor
    ~PeerClient()
    {
        close(sock);
    }
};

bool PeerClient::connect_to_server()
{
    if (connect(sock, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
    {
        perror("Connection to server failed");
        return false;
    }
    std::cout << "Connected to server.\n";
    return true;
}

void PeerClient::register_file(const std::string &filepath)
{
    createChunksFromFile(filepath);
    std::string file_name = std::filesystem::path(filepath).filename().string();
    std::string file_hash = generateHashFromFile(filepath);
    int total_chunks = get_chunk_count(file_hash);

    const int max_chunks_per_message = 4096;
    int chunk_start = 0;

    while (chunk_start < total_chunks)
    {
        // JSON object for this registration message
        json message;
        message["command"] = "REGISTER";
        message["file_hash"] = file_hash;
        message["file_name"] = file_name;
        message["peer_id"] = peer_id;

        // Add chunk IDs in the current batch
        for (int i = chunk_start; i < chunk_start + max_chunks_per_message && i < total_chunks; ++i)
        {
            message["chunks"].push_back(i);
        }

        // Convert JSON to string and send
        std::string message_str = message.dump();
        send(sock, message_str.c_str(), message_str.size(), 0);

        // Receive response
        char buffer[1024] = {0};
        read(sock, buffer, 1024);
        std::cout << "Server response: " << buffer << std::endl;

        chunk_start += max_chunks_per_message;
    }
}

std::vector<std::string> PeerClient::get_available_files()
{
    // Step 1: Create JSON message
    json message;
    message["command"] = "LIST_FILES";

    // Convert JSON to string and send
    std::string message_str = message.dump();
    send(sock, message_str.c_str(), message_str.size(), 0);

    // Step 2: Receive JSON response from server
    char buffer[1024] = {0};
    read(sock, buffer, 1024);

    // Step 3: Parse the JSON response to extract file names
    json response_json;
    std::vector<std::string> files;

    try
    {
        response_json = json::parse(buffer);
        if (response_json["status"] == "success" && response_json.contains("files"))
        {
            files = response_json["files"].get<std::vector<std::string>>();
        }
        else
        {
            std::cerr << "Error: " << response_json["message"] << std::endl;
        }
    }
    catch (json::parse_error &e)
    {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
    }

    return files;
}

void PeerClient::download_file(const std::string &file_hash, int total_chunks)
{
    // Step 1: Send request to server for file chunks
    std::string message = "REQUEST_FILE " + file_hash;
    send(sock, message.c_str(), message.size(), 0);

    // Receive response from server with available chunk details (dummy for now)
    char buffer[1024] = {0};
    read(sock, buffer, 1024);

    std::cout << "Server response: " << buffer << std::endl;

    // Step 2: Download each chunk and reconstruct the file
    for (int i = 0; i < total_chunks; ++i)
    {
        receive_chunk(file_hash, i);
    }

    // Step 3: After downloading all chunks, create the file from chunks
    createFileFromChunks(file_hash);
}

void PeerClient::receive_chunk(const std::string &file_hash, int chunk_id)
{
    std::string message = "LOCATE " + file_hash + " " + std::to_string(chunk_id);
    send(sock, message.c_str(), message.size(), 0);

    char buffer[1024] = {0};
    int bytes_read = read(sock, buffer, 1024);
    if (bytes_read <= 0)
    {
        handle_disconnection();
        return;
    }

    std::string chunk_data(buffer, bytes_read);
    ChunkInfo chunk;
    chunk.chunk_id = chunk_id;
    chunk.data = chunk_data;

    std::ofstream output_file(file_hash + "_" + std::to_string(chunk_id), std::ios::binary);
    output_file << chunk.data;
    output_file.close();

    std::cout << "Received chunk " << chunk_id << " for file " << file_hash << "\n";
}

void PeerClient::handle_disconnection()
{
    std::cerr << "Disconnected from server. Attempting to reconnect...\n";
    close(sock);
    if (!connect_to_server())
    {
        std::cerr << "Failed to reconnect.\n";
        exit(EXIT_FAILURE);
    }
    std::cout << "Reconnected to server.\n";
}

int main()
{
    int peer_id = 1; // Unique ID for this peer
    PeerClient peer("127.0.0.1", 8081, peer_id);

    if (!peer.connect_to_server())
    {
        std::cerr << "Could not connect to server.\n";
        return 1;
    }

    // Register a file to share
    std::string file_path = "./upload/15 Beat It.mp3";
    peer.register_file(file_path); // Now the register_file method is correctly used as a member of PeerClient

    // List available files
    std::vector<std::string> files = peer.get_available_files();
    std::cout << "Available files to download:\n";
    for (const auto &file : files)
    {
        std::cout << " - " << file << "\n";
    }

    // Download a selected file
    std::string file_to_download = files[0]; // Download the first available file
    int total_chunks = get_chunk_count(file_to_download);
    printf("total chunks: %d\n", total_chunks);
    // peer.download_file(file_to_download, total_chunks);

    return 0;
}
