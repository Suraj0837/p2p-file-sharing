#include "include/json.hpp"


#define CHUNK_SIZE 1048576 // 1 MB
#define UPLOAD_FROM_PATH "./upload"
#define CHUNK_FILE_PATH "./chunkked"
#define HASHING_CHUNK_SIZE 4096
#define MAX_RETRIES 5
#define THREAD_POOL_SIZE 100
#define BACKLOG 1000
#define MSG_BUFFER_SIZE 10000

using namespace std;
using json = nlohmann::json;

string generateHashFromFile(string filePath);
void createChunksFromFile(string filepath);
void createFileFromChunks(string hash, string filename);
int get_chunk_count(const std::string &file_hash);
int send_chunk(int connection_fd, int chunk_id, string file_hash);
std::vector<std::pair<std::string, int>> parsePeerList(const std::string& peerListResponse);