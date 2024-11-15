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
    struct sockaddr_in central_server_address;
    std::thread listener_thread;
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
        central_server_address.sin_family = AF_INET;
        central_server_address.sin_port = htons(CENTRAL_SERVER_PORT);
        inet_pton(AF_INET, "127.0.0.1", &central_server_address.sin_addr);
        listener_thread = std::thread(&PeerClient::handle_download_requests, this);
        std::cout<<"peer port:  "<<server_port<<std::endl;
    }

    void handle_download_requests();
    bool connect_to_server();
    bool disconnect_from_server();
    bool is_socket_connected();
    void register_file(const std::string &filepath);
    std::vector<std::pair<std::string, std::string>> get_available_files();
    // void download_file(const std::string &file_hash, int total_chunks);
    void download_file(const std::string &file_hash);
    void receive_chunk(const std::string &file_hash, int chunk_id);
    void handle_disconnection();
    std::string requestPeerListFromServer(std::string file_hash);
    // destructor
    ~PeerClient()
    {
        if (listener_thread.joinable())
        {
            listener_thread.join();
        }
        close(sock);
    }
};

bool PeerClient::connect_to_server()
{
    // Ensure the previous socket is closed properly before creating a new one
    if (sock >= 0)
    {
        close(sock);
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("Socket creation error");
        return false;
    }

    central_server_address.sin_family = AF_INET;
    central_server_address.sin_port = htons(CENTRAL_SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &central_server_address.sin_addr);

    if (connect(sock, (struct sockaddr *)&central_server_address, sizeof(central_server_address)) < 0)
    {
        perror("Connection to server failed");
        return false;
    }

    std::cout << "Connected to server.\n";
    return true;
}

bool PeerClient::disconnect_from_server()
{
    if (sock >= 0 && close(sock) < 0)
    {
        perror("Disconnection from server failed");
        return false;
    }
    sock = -1; // Mark socket as closed
    return true;
}

bool PeerClient::is_socket_connected()
{
    json message;
    message["command"] = "CHECK_CONNECTION";

    // Convert JSON to string and send
    std::string message_str = message.dump();
    
    int result = send(sock, message_str.c_str(), message_str.size(), 0);
    if (result < 0)
    {
        std::cerr << "Socket error or disconnected\n";
        return false;
    }
    return true;
}

void PeerClient::register_file(const std::string &filepath)
{
    createChunksFromFile(filepath);
    std::string file_name = std::filesystem::path(filepath).filename().string();
    std::string file_hash = generateHashFromFile(filepath);
    int total_chunks = get_chunk_count(file_hash);

    const int max_chunks_per_message = 100;
    int chunk_start = 0;

    while (chunk_start < total_chunks)
    {
        // JSON object for this registration message
        json message;
        message["command"] = "REGISTER";
        message["file_hash"] = file_hash;
        message["file_name"] = file_name;
        message["peer_id"] = peer_id;
        message["port"] = server_port;

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

std::vector<std::pair<std::string, std::string>> PeerClient::get_available_files()
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
    std::vector<std::pair<std::string, std::string>> files;

    try
    {
        // Parse the JSON response from the server
        response_json = json::parse(buffer); // Assuming 'buffer' contains the response from the server

        // Check if the response is successful and contains the 'files' array
        if (response_json["status"] == "success" && response_json.contains("files"))
        {
            // Extract files as a vector of pairs (file_hash, file_name)
            for (const auto &file_info : response_json["files"])
            {
                std::string file_hash = file_info["file_hash"];
                std::string file_name = file_info["file_name"];
                files.emplace_back(file_hash, file_name); // Add to the files vector as a pair
            }
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

    return files; // Return the vector of files
}

void PeerClient::handle_download_requests()
{
    int listener_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_sock < 0)
    {
        perror("Socket creation error for peer listener");
        return;
    }

    struct sockaddr_in listener_addr;
    listener_addr.sin_family = AF_INET;
    listener_addr.sin_addr.s_addr = INADDR_ANY;
    listener_addr.sin_port = htons(0); // Let OS pick a port

    if (bind(listener_sock, (struct sockaddr *)&listener_addr, sizeof(listener_addr)) < 0)
    {
        perror("Bind failed for peer listener");
        return;
    }

    listen(listener_sock, 5);

    while (true)
    {
        int client_socket = accept(listener_sock, NULL, NULL);
        if (client_socket >= 0)
        {
            char buffer[1024] = {0};
            read(client_socket, buffer, 1024);
            json request = json::parse(buffer);

            int chunk_id = request["chunk_id"];
            std::string file_hash = request["file_hash"];

            // Simulate sending chunk data
            send_chunk(client_socket, chunk_id, file_hash);

            close(client_socket);
        }
    }
}

std::string PeerClient::requestPeerListFromServer(std::string file_hash)
{
    // Step 1: Send request to server for file chunks
    // Step 1: Create JSON message
    json message;
    message["command"] = "REQUEST_FILE";
    message["file_hash"] = file_hash;

    // Convert JSON to string and send
    std::string message_str = message.dump();
    send(sock, message_str.c_str(), message_str.size(), 0);

    // Step 2: Receive the response from the server
    const int buffer_size = 4096 * 64; // Set a reasonable buffer size
    char buffer[buffer_size];
    std::string response;
    ssize_t bytes_received = recv(sock, buffer, buffer_size - 1, 0);
    response.append(buffer, bytes_received);

    // Loop to read until there’s no more data (when `recv` returns 0 or a negative value)
    // while (true)
    // {
    //     std::cout<<"here:"<<std::endl;
    //     ssize_t bytes_received = recv(sock, buffer, buffer_size - 1, 0);
    //     if (bytes_received <= 0)
    //     {
    //         // If no more data or an error occurs, break the loop
    //         break;
    //     }
    //     // Append received data to the response string
    //     response.append(buffer, bytes_received);
    // }

    std::cout << "Server response: " << response << std::endl;
    return response;
}

void PeerClient::download_file(const std::string &file_hash)
{
    // Request peer list from the server
    std::string response = requestPeerListFromServer(file_hash); // Assume this function is defined elsewhere
    auto peerAddresses = parsePeerList(response);
    json response_parsed = json::parse(response);

    // Connect to each peer and request specific chunks of the file
    for (const auto &[peerIP, peerPort] : peerAddresses)
    {
        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0)
        {
            std::cerr << "Error creating socket\n";
            continue;
        }

        struct sockaddr_in peerAddr;
        peerAddr.sin_family = AF_INET;
        peerAddr.sin_port = htons(peerPort);
        if (inet_pton(AF_INET, peerIP.c_str(), &peerAddr.sin_addr) <= 0)
        {
            std::cerr << "Invalid peer IP address\n";
            close(sockfd);
            continue;
        }

        if (connect(sockfd, (struct sockaddr *)&peerAddr, sizeof(peerAddr)) < 0)
        {
            std::cerr << "Failed to connect to peer: " << peerIP << ":" << peerPort << "\n";
            close(sockfd);
            continue;
        }

        // Request specific chunks (for example, chunk 1, 2, etc.)
        for (int chunkID = 1; chunkID <= response_parsed["total_chunks"]; ++chunkID)
        {
            // Create JSON request with chunk_id and file_hash
            json chunkRequest;
            chunkRequest["chunk_id"] = chunkID;
            chunkRequest["file_hash"] = file_hash;

            // Convert the JSON request to a string and send it
            std::string chunkRequestStr = chunkRequest.dump(); // Serialize JSON to string
            send(sockfd, chunkRequestStr.c_str(), chunkRequestStr.length(), 0);

            // Receive the chunk data
            char buffer[1024];
            std::string chunkData;
            ssize_t bytesRead;

            // Loop to handle receiving large amounts of data
            while ((bytesRead = recv(sockfd, buffer, sizeof(buffer), 0)) > 0)
            {
                chunkData.append(buffer, bytesRead);
            }

            // Save the chunk data (this would normally append to the actual file)
            std::ofstream outFile("_downloaded_chunk_" + std::to_string(chunkID), std::ios::binary | std::ios::app);
            outFile.write(chunkData.data(), chunkData.size());
            outFile.close();

            std::cout << "Downloaded chunk " << chunkID << " from " << peerIP << ":" << peerPort << "\n";
        }

        close(sockfd);
    }
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

void registerFile(PeerClient &peer)
{
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::string file_path;
    std::cout << "Enter file path to register: ";
    std::getline(std::cin, file_path);
    std::cout << file_path << std::endl;

    peer.register_file(file_path);
}

void getAvailableFiles(PeerClient &peer)
{
    std::vector<std::pair<std::string, std::string>> files = peer.get_available_files();
    std::cout << "Available files to download:\n";
    for (const auto &file : files)
{
    std::cout << " - File Hash: " << file.first << " ; File Name: " << file.second << "\n";
}
}

void downloadFile(PeerClient &peer)
{
    std::string filehash;
    std::cout << "Enter filehash to download: ";
    std::cin >> filehash;

    peer.download_file(filehash);
}

void connectAndDownloadChunks(const std::string &filename)
{
    // This is a stub for connecting with peers and downloading chunks.
    // You'll need to implement actual peer-to-peer chunk downloading here.
    std::cout << "Simulating download of chunks for file: " << filename << std::endl;
    // Example: Peer1 downloads chunk 0, Peer2 downloads chunk 1
    std::cout << "Downloading chunk 0 from Peer1...\n";
    std::cout << "Downloading chunk 1 from Peer2...\n";
}

int main()
{
    std::cout << "Enter peer port:\n";
    int port;
    std::cin >> port;
    int peer_id = port; // Unique ID for this peer
    PeerClient peer("127.0.0.1", port, peer_id);

    if (!peer.connect_to_server())
    {
        std::cerr << "Could not connect to server.\n";
        return 1;
    }

    while (true)
    {
        std::cout << "Choose action:\n";
        std::cout << "1. Register file\n";
        std::cout << "2. Get available files\n";
        std::cout << "3. Download file\n";
        std::cout << "4. Check connection status\n"; // New action
        std::cout << "5. Exit\n";
        int choice;
        std::cin >> choice;

        if (std::cin.fail())
        {
            std::cin.clear();                                                   // Clear the error flag
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); // Discard invalid input
            std::cout << "Invalid choice, try again.\n";
            continue; // Skip to the next iteration of the loop
        }

        if (!peer.is_socket_connected())
        {
            std::cout << "Socket is not connected. Reconnecting...\n";
            if (!peer.connect_to_server())
            {
                std::cerr << "Failed to reconnect to the server.\n";
            }
            else
            {
                std::cout << "Reconnected successfully.\n";
            }
        }
        else
        {
            std::cout << "Socket is still connected.\n";
        }

        switch (choice)
        {
        case 1:
            registerFile(peer);
            break;
        case 2:
            getAvailableFiles(peer);
            break;
        case 3:
            downloadFile(peer);
            break;
        case 4:
            if (!peer.is_socket_connected())
            {
                std::cout << "Socket is not connected. Reconnecting...\n";
                if (!peer.connect_to_server())
                {
                    std::cerr << "Failed to reconnect to the server.\n";
                }
                else
                {
                    std::cout << "Reconnected successfully.\n";
                }
            }
            else
            {
                std::cout << "Socket is still connected.\n";
            }
            break;
        case 5:
            return 0;
        default:
            std::cout << "Invalid choice, try again.\n";
        }
    }

    // if (!peer.disconnect_from_server())
    // {
    //     std::cerr << "error occured while disconnecting.\n";
    //     return 1;
    // }

    // Register a file to share
    // std::string file_path = "./upload/15 Beat It.mp3";
    // peer.register_file(file_path); // Now the register_file method is correctly used as a member of PeerClient

    // // List available files
    // std::vector<std::string> files = peer.get_available_files();
    // std::cout << "Available files to download:\n";
    // for (const auto &file : files)
    // {
    //     std::cout << " - " << file << "\n";
    // }

    // // Download a selected file
    // std::string file_to_download = files[0]; // Download the first available file
    // int total_chunks = get_chunk_count(file_to_download);
    // printf("total chunks: %d\n", total_chunks);
    // // peer.download_file(file_to_download, total_chunks);

    return 0;
}
