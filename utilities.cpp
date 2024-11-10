#include <iostream>
#include <fstream>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <string>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <filesystem>
#include <format>
#include <unistd.h>
#include "utilities.h"
#include <cstring>
#include <cerrno>

using namespace std;

int send_chunk(int connection_fd, string message){
    char buffer[CHUNK_SIZE];
    int bytesRead;
    json obj = json::parse(message);
    int chunk_id = obj.at("chunk_id").get<int>();
    string file_hash = obj.at("file_hash").get<string>();
    string chunk_path = "./chunkked/" + file_hash + "/" + to_string(chunk_id) + ".bin";
    
    if(!std::filesystem::exists(chunk_path))
        return -1;

    ifstream inputFile(chunk_path, ios::binary);
    if (!inputFile.is_open()){
        cout << "Unable to open file" << endl;
        return -1;
    }

    inputFile.seekg(0, ios::beg);
    cout << "Connection FD : "<< connection_fd << endl;
    cout << "message " << message << endl;
    
    while (inputFile.good())
    {
        inputFile.read(buffer, CHUNK_SIZE);
        bytesRead = inputFile.gcount();
        getchar();
        // cout << buffer << endl;
        if(write(connection_fd, buffer, CHUNK_SIZE) == -1){
            cout<<"Error while sending chunk "<<chunk_id<< endl;
            std::cerr << "Error writing to file: " << strerror(errno) << std::endl;
            return -1;
        }
        inputFile.close();
    }
    cout << buffer << endl;
    return 0;
}

int get_chunk_count(const std::string &file_hash)
{
    std::string chunk_dir = "./chunkked/" + file_hash; // Assuming chunks are stored in a folder named after the file hash
    int chunk_count = 0;

    // Check if directory exists
    if (std::filesystem::exists(chunk_dir) && std::filesystem::is_directory(chunk_dir))
    {
        // Iterate through the directory and count files
        for (const auto &entry : std::filesystem::directory_iterator(chunk_dir))
        {
            // Only count files (not directories)
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

void createFileFromChunks(string hash)
{
    int chunk_idx = 0;
    char buffer[CHUNK_SIZE];
    ofstream outputFile("./downloads/files/hash", ios::binary);
    if (!outputFile.is_open())
    {
        cout << "Unable to open output file for writing." << endl;
        return;
    }
    // string chunk_file_path = "./chunkked/" + hash + "/" + to_string(chunk_idx) + ".bin";
    string chunk_file_path = "./downloads/chunks/" + hash + "/" + to_string(chunk_idx) + ".bin";
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
        // chunk_file_path = "./chunkked/" + hash + "/" + to_string(chunk_idx) + ".bin";
        chunk_file_path = "./downloads/chunks/" + hash + "/" + to_string(chunk_idx) + ".bin";
    }

    outputFile.close();
    cout << "File reconstruction complete." << endl;
}

// int main()
// {
//     createChunksFromFile("upload/sample.mp4");
//     // cout<<"chunck count: "<<get_chunk_count(generateHashFromFile("upload/My_Oh_My.mp3"))<<endl;
//     createFileFromChunks(generateHashFromFile("upload/sample.mp4"));
//     return 0;
// }