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
#include "threadpool.h"
#include <unordered_map>
#include <deque>
#include <random>

#define CENTRAL_SERVER_PORT 8080
#define MAX_RETRIES 5
#define THREAD_COUNT 10

// things required for multithreaded implementation
struct DownloadInfo{
    // string peer_ip;
    string file_hash;
    int chunk_id;
};



pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t dir_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t server_message_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t retry_count_lock = PTHREAD_MUTEX_INITIALIZER;

pthread_cond_t condition = PTHREAD_COND_INITIALIZER;

deque<struct DownloadInfo*> request_queue;
unordered_map<string, bool> status;
bool kill_threads = false;

unordered_map<string, string> server_message;
unordered_map<string, int> retry_count;

void *downloader_thread(void *){
    while(true){
        
        pthread_mutex_lock(&queue_lock);
        while(request_queue.empty() && !kill_threads)
            pthread_cond_wait(&condition, &queue_lock);

        if(request_queue.empty() && kill_threads)
            pthread_exit(NULL);

        struct DownloadInfo* front = request_queue.front();
        request_queue.pop_front();

        pthread_mutex_unlock(&queue_lock);

        string file_hash = front->file_hash;
        int chunkID = front->chunk_id;

        // Get the peer list for the file hash
        pthread_mutex_lock(&server_message_lock);
            string response = server_message[file_hash];
        pthread_mutex_unlock(&server_message_lock);

        auto peerAddresses = parsePeerList(response); // Now returns both IP and port
        delete front;

        pthread_mutex_lock(&dir_lock);
        filesystem::path directory_path = "./chunkked/" + file_hash;
        if (!(filesystem::exists(directory_path) && filesystem::is_directory(directory_path))){
            if (filesystem::create_directory(directory_path))
                cout << "Directory created successfully." << endl;
            else
                cout << "Directory creation failed or already exists." << endl;
        }
        pthread_mutex_unlock(&dir_lock);

        bool downloadSuccess = false;
        int retryCount = 0;

        // Loop through the peer list until the download is successful or max retries are reached
        for (auto &[peerIP, peerPort] : peerAddresses) {
            cout << "Trying to connect to PeerIP: " << peerIP << " PeerPort: " << peerPort << endl;

            int sockfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd < 0) {
                std::cerr << "Error creating socket\n";
                continue;
            }

            struct sockaddr_in peerAddr;
            peerAddr.sin_family = AF_INET;
            peerAddr.sin_port = htons(peerPort);
            if (inet_pton(AF_INET, peerIP.c_str(), &peerAddr.sin_addr) <= 0) {
                std::cerr << "Invalid peer IP address\n";
                perror("Error details");
                close(sockfd);
                continue;
            }

            // Attempt connection to the peer
            if (connect(sockfd, (struct sockaddr *)&peerAddr, sizeof(peerAddr)) < 0) {
                std::cerr << "Failed to connect to peer: " << peerIP << ":" << peerPort << "\n";
                perror("Error details");
                close(sockfd);
                continue; // Move on to the next peer in the list
            }

            // Create JSON request with chunk_id and file_hash
            json chunkRequest;
            chunkRequest["chunk_id"] = chunkID;
            chunkRequest["file_hash"] = file_hash;

            // Convert the JSON request to a string and send it
            std::string chunkRequestStr = chunkRequest.dump(); // Serialize JSON to string
            send(sockfd, chunkRequestStr.c_str(), chunkRequestStr.length(), 0);
            // debug
            cout << "Request message for chunk " << chunkID << " sent successfully" << endl;

            // Receive the chunk data
            char buffer[1024];
            std::string chunkData;
            ssize_t bytesRead;

            // Loop to handle receiving large amounts of data
            while ((bytesRead = recv(sockfd, buffer, sizeof(buffer), 0)) > 0) {
                chunkData.append(buffer, bytesRead);
            }

            if (chunkData.size()) {
                cout << "Chunk " << chunkID << " received successfully" << endl;
                pthread_mutex_lock(&dir_lock);
                string path = "./chunkked/" + file_hash + "/";
        
                // Save the chunk data (this would normally append to the actual file)
                std::ofstream outFile(path + std::to_string(chunkID) + ".bin", std::ios::binary);
                outFile.write(chunkData.data(), chunkData.size());
                outFile.close();
                pthread_mutex_unlock(&dir_lock);
                std::cout << "Downloaded chunk " << chunkID << " from " << peerIP << ":" << peerPort << "\n";
                close(sockfd);

                pthread_mutex_lock(&status_lock);
                status[file_hash + ":" + to_string(chunkID)] = true;
                pthread_mutex_unlock(&status_lock);
                
                downloadSuccess = true;  // Successfully downloaded
                break;  // Exit the loop as the download was successful
            } else {
                cout << "Chunk " << chunkID << " - failed receiving...retrying" << endl;

                pthread_mutex_lock(&retry_count_lock);
                retry_count[file_hash + ":" + to_string(chunkID)]++;
                int count = retry_count[file_hash + ":" + to_string(chunkID)];
                pthread_mutex_unlock(&retry_count_lock);

                // If maximum retries are done, don't add back to the queue
                if (count == MAX_RETRIES) {
                    break;  // Max retries reached, stop attempting for this chunk
                }

                pthread_mutex_lock(&status_lock);
                status[file_hash + ":" + to_string(chunkID)] = false;
                pthread_mutex_unlock(&status_lock);

                // Re-add to queue for retry from another peer
                pthread_mutex_lock(&queue_lock);
                struct DownloadInfo* request = new DownloadInfo();
                request->chunk_id = chunkID;
                request->file_hash = file_hash;
                // request->peer_ip = peerIP;  // Optional if you want to store peer IP
                request_queue.push_back(request);
                pthread_mutex_unlock(&queue_lock);
            }
        }

        if (!downloadSuccess) {
            cout << "Max retries reached. Chunk " << chunkID << " could not be downloaded." << endl;
        }
    }
}



// void *downloader_thread(void *){
//     std::random_device rd; // Seed
//     std::mt19937 gen(rd()); // Mersenne Twister engine
    
//     while(true){
        
//         pthread_mutex_lock(&queue_lock);
//         while(request_queue.empty() && !kill_threads)
//             pthread_cond_wait(&condition, &queue_lock);

//         if(request_queue.empty() && kill_threads)
//             pthread_exit(NULL);

//         struct DownloadInfo* front = request_queue.front();
//         request_queue.pop_front();

//         pthread_mutex_unlock(&queue_lock);

//         // string peerIP = front->peer_ip;
//         string file_hash = front->file_hash;
//         int chunkID = front->chunk_id;

//         // get random peerIp
//         pthread_mutex_lock(&server_message_lock);
//             string response = server_message[file_hash];
//         pthread_mutex_unlock(&server_message_lock);

//         auto peerAddresses = parsePeerList(response);
//         std::uniform_int_distribution<> dist(0, peerAddresses.size() - 1);
//         auto &[peerIP, ignorePort] = peerAddresses[dist(gen)];
        
//         delete front;

//         pthread_mutex_lock(&dir_lock);
//         filesystem::path directory_path = "./download/" + file_hash;
//         if (!(filesystem::exists(directory_path) && filesystem::is_directory(directory_path))){
//             if (filesystem::create_directory(directory_path))
//                 cout << "Directory created successfully." << endl;
//             else
//                 cout << "Directory creation failed or already exists." << endl;
//         }
//         pthread_mutex_unlock(&dir_lock);

//         // randomly use port between 9000 and 9010
//         // but hardcoded for now
//         int peerPort = 8081;
//         cout <<"Details are : PeerIP: "<< peerIP<<" PeerPort: "<<peerPort << endl;


//         int sockfd = socket(AF_INET, SOCK_STREAM, 0);
//         if (sockfd < 0)
//         {
//             std::cerr << "Error creating socket\n";
//             continue;
//         }

//         struct sockaddr_in peerAddr;
//         peerAddr.sin_family = AF_INET;
//         peerAddr.sin_port = htons(peerPort);
//         if (inet_pton(AF_INET, peerIP.c_str(), &peerAddr.sin_addr) <= 0)
//         {
//             std::cerr << "Invalid peer IP address\n";
//             perror("Error details");
//             close(sockfd);
//             continue;
//         }
//         // cout << "Connection Request sent " << endl;
//         if (connect(sockfd, (struct sockaddr *)&peerAddr, sizeof(peerAddr)) < 0)
//         {
//             std::cerr << "Failed to connect to peer: " << peerIP << ":" << peerPort << "\n";
//             perror("Error details");
//             close(sockfd);
//             continue;
//         }

//         // cout << "Connection Established" << endl;
//         // Create JSON request with chunk_id and file_hash
//         json chunkRequest;
//         chunkRequest["chunk_id"] = chunkID;
//         chunkRequest["file_hash"] = file_hash;

//         // Convert the JSON request to a string and send it
//         std::string chunkRequestStr = chunkRequest.dump(); // Serialize JSON to string
//         send(sockfd, chunkRequestStr.c_str(), chunkRequestStr.length(), 0);
//         // debug
//         cout<<"Request message for chunk "<<chunkID<<" sent successfully"<<endl;

//         // Receive the chunk data
//         char buffer[1024];
//         std::string chunkData;
//         ssize_t bytesRead;

//         // Loop to handle receiving large amounts of data
//         while ((bytesRead = recv(sockfd, buffer, sizeof(buffer), 0)) > 0)
//         {
//             chunkData.append(buffer, bytesRead);
//         }


//         if(chunkData.size()){
//             cout << "Chunk " << chunkID << " recevied successfully" << endl;
//             pthread_mutex_lock(&dir_lock);
//                 string path = "./download/" + file_hash + "/";
        
//                 // Save the chunk data (this would normally append to the actual file)
//                 std::ofstream outFile(path + std::to_string(chunkID) + ".bin", std::ios::binary);
//                 outFile.write(chunkData.data(), chunkData.size());
//                 outFile.close();
//             pthread_mutex_unlock(&dir_lock);
//             std::cout << "Downloaded chunk " << chunkID << " from " << peerIP << ":" << peerPort << "\n";
//             close(sockfd);
            
//             pthread_mutex_lock(&status_lock);
//                 status[file_hash+ ":" + to_string(chunkID)] = true;
//             pthread_mutex_unlock(&status_lock);

//         }
//         else{

//             pthread_mutex_lock(&retry_count_lock);
//                 retry_count[file_hash+":"+to_string(chunkID)]++;
//                 int count = retry_count[file_hash+":"+to_string(chunkID)];
//             pthread_mutex_unlock(&retry_count_lock);
            
//             // if maximum retries are done, then don't add back to the queue
//             if(count == MAX_RETRIES) continue;

//             cout << "Chunk " << chunkID << " - failed receiving...retrying" << endl;
//             pthread_mutex_lock(&status_lock);
//                 status[file_hash+ ":" + to_string(chunkID)] = false;
//             pthread_mutex_unlock(&status_lock);

//             pthread_mutex_lock(&queue_lock);
//                 struct DownloadInfo* request = new DownloadInfo();;
//                 request->chunk_id = chunkID;
//                 request->file_hash = file_hash;
//                 // request->peer_ip = peerIP;

//                 request_queue.push_back(request);
//             pthread_mutex_unlock(&queue_lock);
//         }
//     }
// }



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
    ThreadPool thread_pool; 

public:
    PeerClient(const std::string &ip, int port, int id, size_t pool_size = 4)
        : server_ip(ip), server_port(port), peer_id(id), thread_pool(pool_size)
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

    void handle_connection(int client_socket);
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
    std::string getFileName(const std::string &filehash);
    std::string requestPeerListFromServer(std::string file_hash);
    void registerPeer(const std::string &file_hash, int peer_id, int peer_port);
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

std::string PeerClient::getFileName(const std::string &filehash)
{
    json message;
    message["command"] = "GET_FILE_NAME";
    message["file_hash"] = filehash; // Add file hash to the request

    // Convert JSON to string and send
    std::string message_str = message.dump();
    int result = send(sock, message_str.c_str(), message_str.size(), 0);
    if (result < 0)
    {
        std::cerr << "Socket error or disconnected\n";
        return ""; // Return empty string on error
    }

    // Receive the response
    char buffer[1024] = {0};
    int bytes_read = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0)
    {
        std::cerr << "Failed to receive response from server\n";
        return ""; // Return empty string on error
    }

    // Parse the response
    try
    {
        json response = json::parse(std::string(buffer, bytes_read));
        if (response["status"] == "success")
        {
            return response["file_name"]; // Return the file name on success
        }
        else
        {
            std::cerr << "Error: " << response["message"] << "\n";
            return ""; // Return empty string on failure
        }
    }
    catch (json::parse_error &e)
    {
        std::cerr << "Invalid JSON response: " << e.what() << "\n";
        return ""; // Return empty string on error
    }
}

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

// void PeerClient::handle_download_requests()
// {
//     int listener_sock = socket(AF_INET, SOCK_STREAM, 0);
//     if (listener_sock < 0)
//     {
//         perror("Socket creation error for peer listener");
//         return;
//     }

//     struct sockaddr_in listener_addr;
//     listener_addr.sin_family = AF_INET;
//     listener_addr.sin_addr.s_addr = INADDR_ANY;
//     // listener_addr.sin_port = htons(0); // Let OS pick a port
//     listener_addr.sin_port = htons(this->server_port);

//     if (bind(listener_sock, (struct sockaddr *)&listener_addr, sizeof(listener_addr)) < 0)
//     {
//         perror("Bind failed for peer listener");
//         return;
//     }

//     listen(listener_sock, 5);

//     while (true)
//     {
//         int client_socket = accept(listener_sock, NULL, NULL);
//         if (client_socket >= 0)
//         {
//             char buffer[1024] = {0};
//             read(client_socket, buffer, 1024);
//             cout<<"Receieved message "<< buffer<< endl;

//             json request = json::parse(buffer);

//             int chunk_id = request["chunk_id"];
//             std::string file_hash = request["file_hash"];

//             // cout<<"Extracted Chunk id: " << chunk_id << "Extracted File hash: "<< file_hash << endl;
//             // Simulate sending chunk data
//             // cout <<"Invoking send chunk"<<endl;
//             send_chunk(client_socket, chunk_id, file_hash);
//             cout << "Data sent successfully...Closing connection"<< endl;
//             close(client_socket);
//         }
//     }
// }

void PeerClient::handle_download_requests() {
    int listener_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_sock < 0) {
        perror("Socket creation error for peer listener");
        return;
    }

    struct sockaddr_in listener_addr;
    listener_addr.sin_family = AF_INET;
    listener_addr.sin_addr.s_addr = INADDR_ANY;
    listener_addr.sin_port = htons(this->server_port);

    if (bind(listener_sock, (struct sockaddr *)&listener_addr, sizeof(listener_addr)) < 0) {
        perror("Bind failed for peer listener");
        return;
    }

    listen(listener_sock, 5);

    while (true) {
        int client_socket = accept(listener_sock, NULL, NULL);
        if (client_socket >= 0) {
            // Enqueue the connection to the thread pool
            thread_pool.enqueue([this, client_socket] {
                handle_connection(client_socket);
            });
        }
    }
}

void PeerClient::handle_connection(int client_socket) {
    char buffer[1024] = {0};
    read(client_socket, buffer, 1024);
    std::cout << "Received message: " << buffer << std::endl;

    json request = json::parse(buffer);
    int chunk_id = request["chunk_id"];
    std::string file_hash = request["file_hash"];

    // Simulate sending chunk data
    send_chunk(client_socket, chunk_id, file_hash);
    std::cout << "Data sent successfully... Closing connection" << std::endl;
    close(client_socket);
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

void PeerClient::registerPeer(const std::string &file_hash, int peer_id, int peer_port)
{
    json message;
    message["command"] = "REGISTER_PEER";
    message["file_hash"] = file_hash;
    message["peer_id"] = peer_id;
    message["port"] = peer_port;

    std::string message_str = message.dump();
    int result = send(sock, message_str.c_str(), message_str.size(), 0);
    if (result < 0)
    {
        std::cerr << "Failed to send REGISTER_PEER request\n";
        return;
    }

    char buffer[1024] = {0};
    int bytes_read = recv(sock, buffer, sizeof(buffer), 0);
    if (bytes_read > 0)
    {
        std::cout << "Response from server: " << std::string(buffer, bytes_read) << std::endl;
    }
}


void PeerClient::download_file(const std::string &file_hash)
{
    // Request peer list from the server
    std::string response = requestPeerListFromServer(file_hash); // Assume this function is defined elsewhere
    auto peerAddresses = parsePeerList(response);
    json response_parsed = json::parse(response);

    pthread_mutex_lock(&server_message_lock);
        server_message[file_hash] = response;
    pthread_mutex_unlock(&server_message_lock);

    // hardcoding temporarily
    // auto &[peerIP, peerPort] = peerAddresses[0];

    //set status map
    pthread_mutex_lock(&status_lock);
        for(int chunkId = 0; chunkId < response_parsed["total_chunks"]; chunkId++){
            status[file_hash+":"+to_string(chunkId)] = false;
        }
    pthread_mutex_unlock(&status_lock);   

    //set retry count map
    pthread_mutex_lock(&retry_count_lock);
        for(int chunkId = 0; chunkId < response_parsed["total_chunks"]; chunkId++){
            retry_count[file_hash+":"+to_string(chunkId)] = 0;
        }
    pthread_mutex_unlock(&retry_count_lock);   

    // initialize threads
    pthread_t threads[THREAD_COUNT];
    for(int i = 0; i < THREAD_COUNT; i++){
        int rc = pthread_create(threads+i, NULL, &downloader_thread, NULL);
        if (rc != 0){
            std::cerr << "Error creating thread " << i << ": " << strerror(rc) << std::endl;
        }
    }
    cout<<"Threads are created"<<endl;


    for(int chunkId = 0; chunkId < response_parsed["total_chunks"]; chunkId++){
        struct DownloadInfo* request = new DownloadInfo();
        request->chunk_id = chunkId;
        request->file_hash = file_hash;
        // request->peer_ip = peerIP;

        pthread_mutex_lock(&queue_lock);
            request_queue.push_back(request);
            pthread_cond_signal(&condition);
        pthread_mutex_unlock(&queue_lock);
    }
    cout << "All requests are uploaded" << endl;

    int missingFlag = 0;

    for(int chunkId = 0; chunkId < response_parsed["total_chunks"]; chunkId++){
        cout << "Waiting for chunk: "<< chunkId << "....";
        while(!status[file_hash+":"+to_string(chunkId)] && retry_count[file_hash+":"+to_string(chunkId)] != MAX_RETRIES);
        if(status[file_hash+":"+to_string(chunkId)])
            cout << "Received" << endl;
        else{
            missingFlag = 1;
            cout << "Failed to download chunk "<< chunkId << endl;
        }
           
    }

    if(!missingFlag) {
        std::string filename = getFileName(file_hash);
        createFileFromChunks(file_hash, filename);
        registerPeer(file_hash, peer_id, server_port);
    }

    // erase all the relevant entries from status map
    for(int chunkId = 0; chunkId < response_parsed["total_chunks"]; chunkId++){
        status.erase(file_hash+":"+to_string(chunkId));
        retry_count.erase(file_hash+":"+to_string(chunkId));
    }

    

    kill_threads = true;
    pthread_cond_broadcast(&condition);
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

void getfilename(PeerClient &peer)
{
    std::string filehash;
    std::cout << "Enter filehash:";
    std::cin >> filehash;

    std::string filename = peer.getFileName(filehash);

    cout << "file name: " << filename << endl;
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
        std::cout << "5. Get file name\n"; // New action
        std::cout << "6. Exit\n";
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
            getfilename(peer);
            break;
        case 6:
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
