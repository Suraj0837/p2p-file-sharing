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
#include <random>
#include <pthread.h>
#include <deque>


#define PEER_SERVER_PORT 8080


using json = nlohmann::json;

// threads code for peer being server
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition = PTHREAD_COND_INITIALIZER;



// Data Structures
struct RequestInfo{
    int conn_fd;
    string message;
};
struct ChunkInfo
{
    int chunk_id;
    std::string data;
};

struct PeerInfo{
    int peerId;
    string ip;
    int port;
};

deque<struct RequestInfo*> tasks;
void* listener_thread(void *){
    int sock_fd, connection_fd;
    struct sockaddr_in server_address, client_address;
    char buffer[MSG_BUFFER_SIZE];
    socklen_t length = sizeof(client_address);

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(sock_fd < 0){
        printf("Socket Creation failed\n");
        exit(1);
    }

    
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(PEER_SERVER_PORT);

    if(bind(sock_fd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0){
        printf("Bind Failed\n");
        exit(2);
    }

    if(listen(sock_fd, BACKLOG) < 0){
        printf("Listen Failed\n");
        exit(3);    
    }
    while(1){
        connection_fd = accept(sock_fd, (struct sockaddr *)&client_address, &length);
        if(connection_fd < 0){
            printf("%d", connection_fd);
            printf("Accept failed\n");
            continue;
        }

        // TODO: optimize here, by adding epoll so every read does not need to block and wait
        int bytesRead = read(connection_fd, &buffer, MSG_BUFFER_SIZE);
        buffer[bytesRead] = '\0';

        struct RequestInfo* request = (struct RequestInfo*)malloc(sizeof(struct RequestInfo));
        request->conn_fd = connection_fd;
        request->message = buffer;

        pthread_mutex_lock(&queue_lock);
        tasks.emplace_back(request);
        pthread_cond_signal(&condition);
        pthread_mutex_unlock(&queue_lock);
    }
}

void* worker_thread(void *){
    while (1){
        pthread_mutex_lock(&queue_lock);
        while(tasks.empty())
            pthread_cond_wait(&condition, &queue_lock);

        struct RequestInfo* request = tasks.front();
        tasks.pop_front();
        pthread_mutex_unlock(&queue_lock);

        string msg = request->message;
        int conn = request->conn_fd;

        cout << "Message" << msg;
        cout << "Connection" << conn;

        free(request);
        json object = json::parse(msg);

        if(object["command"] == "REQUEST_CHUNK")
            send_chunk(conn, msg);
    }

}



class PeerClient
{
private:
    int sock;
    int peer_id;
    struct sockaddr_in server_address;
    std::string server_ip;
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
        server_address.sin_port = htons(server_port);
        inet_pton(AF_INET, server_ip.c_str(), &server_address.sin_addr);
    }

    bool connect_to_server();
    void register_file(const std::string &filepath);
    // int get_chunk_count(const std::string &file_hash); // Added method to get chunk count
    std::vector<std::string> get_available_files();
    void download_file(const std::string &file_hash);
    void receive_chunk(const std::string &file_hash, int chunk_id, vector<PeerInfo>);
    void handle_disconnection();
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
    // Step 1: Create chunks from file
    createChunksFromFile(filepath);
    std::string file_hash = generateHashFromFile(filepath);
    int total_chunks = get_chunk_count(file_hash); // Getting the chunk count

    // Step 2: Register the chunks in batches of up to 4096
    const int max_chunks_per_message = 4096;
    int chunk_start = 0;

    while (chunk_start < total_chunks)
    {
        // Create a message for a batch of up to 4096 chunk IDs
        std::string message = "REGISTER " + file_hash + " " + std::to_string(peer_id);

        // Add up to 4096 chunk ids to the message
        for (int i = chunk_start; i < chunk_start + max_chunks_per_message && i < total_chunks; ++i)
        {
            message += " " + std::to_string(i);
        }

        // Send the message to the server
        send(sock, message.c_str(), message.size(), 0);

        // Receive response from the server
        char buffer[1024] = {0};
        read(sock, buffer, 1024);
        std::cout << "Server response: " << buffer << std::endl;

        // Update the start index for the next batch
        chunk_start += max_chunks_per_message;
    }
}

std::vector<std::string> PeerClient::get_available_files()
{
    std::string message = "LIST_FILES";
    send(sock, message.c_str(), message.size(), 0);

    char buffer[1024] = {0};
    read(sock, buffer, 1024);
    std::vector<std::string> files;
    std::istringstream response(buffer);
    std::string file_hash;
    while (response >> file_hash)
    {
        files.push_back(file_hash);
    }
    return files;
}

void PeerClient::download_file(const std::string &file_hash)
{
    // Step 1: Send request to server for file chunks
    // std::string message = "REQUEST_FILE " + file_hash;
    // send(sock, message.c_str(), message.size(), 0);

    // right now hardcoding the server response, but need to send the request to server and get response back
    string server_message = R"(
        {
            "response": "PEER_LIST",
            "file_hash": "11011423b6cc2389f331733d845290de09d089be6c2f37b3f9dbf2364f3509fd",
            "total_chunks": 14,
            "chunk_peers": {
                "0": [{"peer_id": 1, "ip": "0.0.0.0", "port": 8000}],
                "1": [{"peer_id": 1, "ip": "0.0.0.0", "port": 8000}],
                "2": [{"peer_id": 1, "ip": "0.0.0.0", "port": 8000}],
                "3": [{"peer_id": 1, "ip": "0.0.0.0", "port": 8000}],
                "4": [{"peer_id": 1, "ip": "0.0.0.0", "port": 8000}],
                "5": [{"peer_id": 1, "ip": "0.0.0.0", "port": 8000}],
                "6": [{"peer_id": 1, "ip": "0.0.0.0", "port": 8000}],
                "7": [{"peer_id": 2, "ip": "0.0.0.0", "port": 8080}],
                "8": [{"peer_id": 2, "ip": "0.0.0.0", "port": 8080}],
                "9": [{"peer_id": 2, "ip": "0.0.0.0", "port": 8080}],
                "10": [{"peer_id": 2, "ip": "0.0.0.0", "port": 8080}],
                "11": [{"peer_id": 2, "ip": "0.0.0.0", "port": 8080}],
                "12": [{"peer_id": 2, "ip": "0.0.0.0", "port": 8080}],
                "13": [{"peer_id": 2, "ip": "0.0.0.0", "port": 8080}, {"peer_id": 1, "ip": "0.0.0.0", "port": 8000}]
            }
        }
)";
    // cout<<"server_message\n"<<server_message<<endl;
    json object = json::parse(server_message);
    // cout<<object<<endl;

    if(object["file_hash"] != file_hash){
        cout<< "Invalid response from server: Mismatching file hash in chunk list" << endl;
        return;
    }
    
    for(int chunk_id = 0; chunk_id < object["total_chunks"]; chunk_id++){
        // creating list for every chunk
        vector<PeerInfo> peer_list;
        try {
            for (const auto& peer : object["chunk_peers"][to_string(chunk_id)]) {
                PeerInfo info;
                info.peerId = peer.at("peer_id").get<int>();
                info.ip = peer.at("ip").get<std::string>();
                info.port = peer.at("port").get<int>();
                peer_list.push_back(info);
            }
        }
        catch (const json::exception& e) {
            std::cerr << "JSON error: " << e.what() << std::endl;
        }

        receive_chunk(file_hash, chunk_id, peer_list);

    }
    // Step 3: After downloading all chunks, create the file from chunks
    createFileFromChunks(file_hash);
}

void PeerClient::receive_chunk(const std::string &file_hash, int chunk_id, vector<PeerInfo> peer_list)
{
    //
    json message;
    message["file_hash"] = file_hash;
    message["chunk_id"] =  chunk_id;
    message["command"] = "REQUEST_CHUNK";

    string stringify = message.dump();

    int retry_count = 0;
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> distrib(0, peer_list.size() - 1);
    char buffer[CHUNK_SIZE];
    int bytesRead;

    // get the chunk, try random peers, try upto MAX_RETRIES
    while(true){
        int random_index = distrib(gen);
        PeerInfo serverInfo = peer_list[random_index];

        //this also connects to server
        PeerClient peer(serverInfo.ip, serverInfo.port, serverInfo.peerId);

        if (!peer.connect_to_server()){
            std::cerr << "Could not connect to server...Retrying with another server\n";
            retry_count++;
            if(retry_count == MAX_RETRIES){
                cout<<"error while collecting all chunks...maximum retries exceeded"<<endl;
                exit(1);
            }
            continue;
        }
        
        // send message to server
        // cout << stringify << endl;
        if(write(peer.sock, stringify.c_str(), stringify.size()) == -1){
            std::cerr << "Could not connect to server...Retrying with another server\n";
            retry_count++;
            if(retry_count == MAX_RETRIES){
                cout<<"error while collecting all chunks...maximum retries exceeded"<<endl;
                exit(1);
            }
            continue;
        }
        
        // wait for the response
        int bytes_read = read(peer.sock, buffer, CHUNK_SIZE);
        if (bytes_read <= 0){
            retry_count++;
            if(retry_count == MAX_RETRIES){
                cout<<"error while collecting all chunks...maximum retries exceeded"<<endl;
                exit(1);
            }
            continue;
        }

        filesystem::path directory_path = "./downloads/chunks/" + file_hash;
        string dir_path = "./downloads/chunks/" + file_hash;
        // if directory exists then delete it completely
        if(!filesystem::exists(directory_path)){
            // create fresh directory
            if (filesystem::create_directory(directory_path))
                cout << "Directory created successfully." << endl;
            else
                cout << "Directory creation failed or already exists." << endl;
        }

        string chunk_file_path = dir_path + "/" + to_string(chunk_id) + ".bin";
        ofstream chunkFile(chunk_file_path, ios::binary);
        chunkFile.write(buffer, bytesRead);
        chunkFile.close();
        
        // if all things are executed correctly then break out of the loop
        cout <<"Chunk "<<chunk_id<<" downloaded"<<endl; 
        break;
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

int main()
{   
    // creating socket to connect with tracker
    int peer_id = 1; // Unique ID for this peer
    PeerClient peer("127.0.0.1", 8080, peer_id);

    // if (!peer.connect_to_server())
    // {
    //     std::cerr << "Could not connect to server.\n";
    //     return 1;
    // }

    // Register a file to share
    // std::string file_path = "./upload/My_Oh_My.mp3";
    // peer.register_file(file_path); // Now the register_file method is correctly used as a member of PeerClient

    // // List available files
    // std::vector<std::string> files = peer.get_available_files();
    // std::cout << "Available files to download:\n";
    // for (const auto &file : files)
    // {
    //     std::cout << " - " << file << "\n";
    // }

    // Download a selected file
    // std::string file_to_download = files[0]; // Download the first available file

    // int total_chunks = peer.get_chunk_count(file_to_download);

    // TODO: add the mapping between hashing and filename,by calling get_available files

    // start the listener thread
    pthread_t Listener;
    pthread_create(&Listener, nullptr, &listener_thread, nullptr);

    // start the worker threads
    pthread_t threads[THREAD_POOL_SIZE];
    for(int i = 0; i < THREAD_POOL_SIZE; i++){
        pthread_create(threads + i, nullptr,&worker_thread, nullptr);
    }

    // string download_file_hash = "11011423b6cc2389f331733d845290de09d089be6c2f37b3f9dbf2364f3509fd";
    // peer.download_file(download_file_hash);

    while(1);

    return 0;
}