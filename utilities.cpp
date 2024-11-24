#include <iostream>
#include <fstream>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <string>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <filesystem>
// #include <format>
#include <unistd.h>
#include "utilities.h"
#include <cstring>
#include <cerrno>


#define UPLOAD_FROM_PATH "./upload"
#define CHUNK_FILE_PATH "./chunkked"
#define HASHING_CHUNK_SIZE 4096

using namespace std;

#include <filesystem>
#include <iostream>

// Parses the peer list and returns a vector of <IP, Port> pairs, ensuring uniqueness
// std::vector<std::pair<std::string, int>> parsePeerList(const std::string& peerListResponse) {
//     std::vector<std::pair<std::string, int>> peerAddresses;

//     // Parse the JSON response
//     json response = json::parse(peerListResponse);

//     // Check if the response has "chunks" key and iterate over it
//     if (response.contains("chunks")) {
//         for (const auto& chunk : response["chunks"]) {
//             // Iterate over peers for each chunk
//             for (const auto& peer : chunk["peers"]) {
//                 std::string ip = peer["ip"];
//                 int port = peer["port"];

//                 // Check if the peer (ip, port) is already in the list
//                 bool alreadyExists = false;
//                 for (const auto& existingPeer : peerAddresses) {
//                     if (existingPeer.first == ip && existingPeer.second == port) {
//                         alreadyExists = true;
//                         break;
//                     }
//                 }

//                 // If it doesn't exist, add it to the vector
//                 if (!alreadyExists) {
//                     peerAddresses.emplace_back(ip, port);
//                 }
//             }
//         }
//     }

//     // Print the unique peer list
//     std::cout << "Unique Peer list:" << std::endl;
//     for (const auto& peer : peerAddresses) {
//         std::cout << "IP: " << peer.first << ", Port: " << peer.second << std::endl;
//     }

//     return peerAddresses;
// }

// Parses the peer list and returns a vector of <IP, Port> pairs, ensuring unique ports
std::vector<std::pair<std::string, int>> parsePeerList(const std::string& peerListResponse) {
    std::vector<std::pair<std::string, int>> peerAddresses;

    // Parse the JSON response
    json response = json::parse(peerListResponse);

    // Check if the response has "chunks" key and iterate over it
    if (response.contains("chunks")) {
        for (const auto& chunk : response["chunks"]) {
            // Iterate over peers for each chunk
            for (const auto& peer : chunk["peers"]) {
                std::string ip = peer["ip"];
                int port = peer["port"];

                // Check if the port is already in the list
                bool alreadyExists = false;
                for (const auto& existingPeer : peerAddresses) {
                    if (existingPeer.second == port) {  // Check only the port for uniqueness
                        alreadyExists = true;
                        break;
                    }
                }

                // If the port doesn't exist, add the peer to the list
                if (!alreadyExists) {
                    peerAddresses.emplace_back(ip, port);
                }
            }
        }
    }

    // Print the unique peer list based on ports
    std::cout << "Unique Peer list (by port):" << std::endl;
    for (const auto& peer : peerAddresses) {
        std::cout << "IP: " << peer.first << ", Port: " << peer.second << std::endl;
    }

    return peerAddresses;
}

int send_chunk(int connection_fd, int chunk_id, string file_hash){
    char buffer[CHUNK_SIZE];
    int bytesRead;
    string chunk_path = "./chunkked/" + file_hash + "/" + to_string(chunk_id) + ".bin";
    
    if(!std::filesystem::exists(chunk_path)){
        cout << "Asked chunk path not found."<<endl; 
        return -1;
    }
        

    ifstream inputFile(chunk_path, ios::binary);
    if (!inputFile.is_open()){
        cout << "Unable to open file" << endl;
        return -1;
    }

    inputFile.seekg(0, ios::beg);
    
    while (inputFile.good())
    {   
        inputFile.read(buffer, CHUNK_SIZE);
        bytesRead = inputFile.gcount();
        if(write(connection_fd, buffer, CHUNK_SIZE) == -1){
            cout<<"Error while sending chunk "<<chunk_id<< endl;
            std::cerr << "Error writing to file: " << strerror(errno) << std::endl;
            return -1;
        }
        inputFile.close();
    }
    // cout << buffer << endl;
    return 0;
}

int get_chunk_count(const std::string &file_hash)
{
    std::string chunk_dir = "./chunkked/" + file_hash;
    int chunk_count = 0;

    if (std::filesystem::exists(chunk_dir) && std::filesystem::is_directory(chunk_dir))
    {
        for (const auto &entry : std::filesystem::directory_iterator(chunk_dir))
        {
            if (std::filesystem::is_regular_file(entry))
            {
                ++chunk_count;
            }
        }
    }
    else
    {
        std::cerr << "Directory not found: " << chunk_dir << "\n";
    }

    return chunk_count;
}


string generateHashFromFile(string filePath)
{
    ifstream file(filePath, ios::binary);

    if (!file.is_open())
    {
        cout << "Unable to open file" << endl;
        return "";
    }

    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context)
    {
        cout << "Failed to create EVP_MD_CTX" << endl;
        return "";
    }

    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1)
    {
        EVP_MD_CTX_free(context);
        cout << "EVP_DigestInit_ex Failed" << endl;
        return "";
    }

    char buffer[HASHING_CHUNK_SIZE];
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_length;

    file.seekg(0, ios::beg);

    while (file.good())
    {
        file.read(buffer, HASHING_CHUNK_SIZE);
        int bytesRead = file.gcount();
        if (EVP_DigestUpdate(context, buffer, bytesRead) != 1)
        {
            EVP_MD_CTX_free(context);
            cout << "Error while updating the EVP CTX" << endl;
            return "";
        }
    }

    if (EVP_DigestFinal_ex(context, hash, &hash_length) != 1)
    {
        EVP_MD_CTX_free(context);
        cout << "EVP_DigestFinal_ex Failed" << endl;
    }

    stringstream hashString;
    for (int i = 0; i < hash_length; i++)
    {
        hashString << hex << setw(2) << setfill('0') << (int)hash[i];
    }
    return hashString.str();
}

void createChunksFromFile(string filepath)
{
    cout<<"creating chunks: "<<endl;
    string hash = generateHashFromFile(filepath);
    int bytesRead;
    int chunk_idx = 0;

    char buffer[CHUNK_SIZE];

    filesystem::path directory_path = "./chunkked/" + hash;

    // if directory exists then delete it completely
    if (filesystem::exists(directory_path) && filesystem::is_directory(directory_path))
        filesystem::remove_all(directory_path);

    // create fresh directory
    if (filesystem::create_directory(directory_path))
        cout << "Directory created successfully." << endl;
    else
        cout << "Directory creation failed or already exists." << endl;

    ifstream inputFile(filepath, ios::binary);
    if (!inputFile.is_open())
    {
        cout << "Unable to open file" << endl;
        return;
    }
    inputFile.seekg(0, ios::beg);

    while (inputFile.good())
    {
        inputFile.read(buffer, CHUNK_SIZE);
        bytesRead = inputFile.gcount();
        string chunk_file_path = "./chunkked/" + hash + "/" + to_string(chunk_idx) + ".bin";
        ofstream chunkFile(chunk_file_path, ios::binary);
        chunkFile.write(buffer, bytesRead);
        chunk_idx++;
        chunkFile.close();
    }

    inputFile.close();
}



void createFileFromChunks(string hash, string filename)
{
    int chunk_idx = 0;
    char buffer[CHUNK_SIZE];
    ofstream outputFile("./" + filename, ios::binary);
    if (!outputFile.is_open())
    {
        cout << "Unable to open output file for writing." << endl;
        return;
    }
    string chunk_file_path = "./chunkked/" + hash + "/" + to_string(chunk_idx) + ".bin";

    while (filesystem::exists(chunk_file_path))
    {
        ifstream chunkFile(chunk_file_path, ios::binary);
        if (!chunkFile.is_open())
        {
            cout << "Unable to open chunk file" << endl;
            return;
        }

        chunkFile.read(buffer, CHUNK_SIZE);

        outputFile.write(buffer, chunkFile.gcount());
        if (!outputFile.good())
        {
            cout << "Error writing to output file." << endl;
            chunkFile.close();
            outputFile.close();
            return;
        }

        chunkFile.close();
        chunk_idx++;
        chunk_file_path = "./chunkked/" + hash + "/" + to_string(chunk_idx) + ".bin";
    }

    outputFile.close();
    cout << "File reconstruction complete." << endl;
}
// int main()
// {
//     createChunksFromFile("upload/My_Oh_My.mp3");
//     cout<<"chunck count: "<<get_chunk_count(generateHashFromFile("upload/My_Oh_My.mp3"))<<endl;
//     createFileFromChunks(generateHashFromFile("upload/My_Oh_My.mp3"));
//     return 0;
// }