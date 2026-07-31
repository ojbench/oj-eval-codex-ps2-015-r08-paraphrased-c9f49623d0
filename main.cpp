#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace std;

namespace {

constexpr uint64_t kBucketCount = 1u << 16;
constexpr char kMagic[8] = {'F', 'S', 'T', 'O', 'R', 'E', '1', '\0'};
constexpr const char* kStorageFile = "file_storage.bin";

#pragma pack(push, 1)
struct Header {
    char magic[8];
    uint64_t bucket_count;
    uint64_t next_offset;
};

struct Node {
    uint8_t type;
    uint8_t reserved1[7];
    uint64_t next;
    uint64_t aux;
    uint32_t len;
    int32_t value;
    char key[64];
};
#pragma pack(pop)

static_assert(sizeof(Header) == 24);
static_assert(sizeof(Node) == 96);

class Storage {
  public:
    Storage() {
        open_or_create();
    }

    ~Storage() {
        if (file_) {
            flush_header_and_buckets();
            std::fclose(file_);
        }
    }

    void insert(const string& key, int value) {
        const uint64_t bucket = hash_key(key) % bucket_count_;
        uint64_t key_offset = find_key_offset(bucket, key);
        if (key_offset == 0) {
            const uint64_t value_offset = allocate_node();
            Node value_node{};
            value_node.type = 2;
            value_node.next = 0;
            value_node.value = value;
            write_node(value_offset, value_node);

            const uint64_t new_key_offset = allocate_node();
            Node key_node{};
            key_node.type = 1;
            key_node.next = bucket_heads_[bucket];
            key_node.aux = value_offset;
            key_node.len = static_cast<uint32_t>(key.size());
            std::memcpy(key_node.key, key.data(), key.size());
            write_node(new_key_offset, key_node);

            bucket_heads_[bucket] = new_key_offset;
            write_bucket_head(bucket, new_key_offset);
            return;
        }

        Node key_node = read_node(key_offset);
        const uint64_t value_offset = allocate_node();
        Node value_node{};
        value_node.type = 2;
        value_node.next = key_node.aux;
        value_node.value = value;
        write_node(value_offset, value_node);

        key_node.aux = value_offset;
        write_node(key_offset, key_node);
    }

    void erase(const string& key, int value) {
        const uint64_t bucket = hash_key(key) % bucket_count_;
        const uint64_t key_offset = find_key_offset(bucket, key);
        if (key_offset == 0) {
            return;
        }

        Node key_node = read_node(key_offset);
        uint64_t current = key_node.aux;
        uint64_t previous = 0;
        while (current != 0) {
            Node node = read_node(current);
            if (node.type == 2 && node.value == value) {
                if (previous == 0) {
                    key_node.aux = node.next;
                    write_node(key_offset, key_node);
                } else {
                    Node prev_node = read_node(previous);
                    prev_node.next = node.next;
                    write_node(previous, prev_node);
                }
                return;
            }
            previous = current;
            current = node.next;
        }
    }

    vector<int> find(const string& key) {
        const uint64_t bucket = hash_key(key) % bucket_count_;
        const uint64_t key_offset = find_key_offset(bucket, key);
        vector<int> result;
        if (key_offset == 0) {
            return result;
        }

        Node key_node = read_node(key_offset);
        uint64_t current = key_node.aux;
        while (current != 0) {
            Node node = read_node(current);
            if (node.type == 2) {
                result.push_back(node.value);
            }
            current = node.next;
        }
        sort(result.begin(), result.end());
        return result;
    }

  private:
    FILE* file_ = nullptr;
    Header header_{};
    uint64_t bucket_count_ = kBucketCount;
    vector<uint64_t> bucket_heads_;

    static uint64_t hash_key(const string& key) {
        uint64_t hash = 1469598103934665603ull;
        for (unsigned char c : key) {
            hash ^= c;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    void open_or_create() {
        const bool exists = std::filesystem::exists(kStorageFile);
        file_ = std::fopen(kStorageFile, exists ? "r+b" : "w+b");
        if (!file_) {
            std::perror("fopen");
            std::exit(1);
        }

        if (exists && load_existing()) {
            return;
        }

        initialize_fresh();
    }

    bool load_existing() {
        if (std::fseek(file_, 0, SEEK_SET) != 0) {
            return false;
        }
        if (std::fread(&header_, sizeof(header_), 1, file_) != 1) {
            return false;
        }
        if (std::memcmp(header_.magic, kMagic, sizeof(kMagic)) != 0) {
            return false;
        }
        if (header_.bucket_count != kBucketCount) {
            return false;
        }

        bucket_count_ = header_.bucket_count;
        next_free_offset_ = header_.next_offset;
        bucket_heads_.assign(bucket_count_, 0);
        if (std::fread(bucket_heads_.data(), sizeof(uint64_t), bucket_count_, file_) != bucket_count_) {
            return false;
        }
        return true;
    }

    void initialize_fresh() {
        bucket_count_ = kBucketCount;
        bucket_heads_.assign(bucket_count_, 0);
        std::memcpy(header_.magic, kMagic, sizeof(kMagic));
        header_.bucket_count = bucket_count_;
        header_.next_offset = sizeof(Header) + bucket_count_ * sizeof(uint64_t);

        std::fseek(file_, 0, SEEK_SET);
        std::fwrite(&header_, sizeof(header_), 1, file_);
        std::fwrite(bucket_heads_.data(), sizeof(uint64_t), bucket_count_, file_);
    }

    void flush_header_and_buckets() {
        header_.next_offset = next_free_offset_;
        std::fseek(file_, 0, SEEK_SET);
        std::fwrite(&header_, sizeof(header_), 1, file_);
        std::fwrite(bucket_heads_.data(), sizeof(uint64_t), bucket_count_, file_);
        std::fflush(file_);
    }

    uint64_t next_free_offset_ = sizeof(Header) + kBucketCount * sizeof(uint64_t);

    uint64_t allocate_node() {
        const uint64_t offset = next_free_offset_;
        next_free_offset_ += sizeof(Node);
        return offset;
    }

    void write_bucket_head(uint64_t bucket, uint64_t offset) {
        bucket_heads_[bucket] = offset;
        const uint64_t file_offset = sizeof(Header) + bucket * sizeof(uint64_t);
        std::fseek(file_, static_cast<long>(file_offset), SEEK_SET);
        std::fwrite(&offset, sizeof(uint64_t), 1, file_);
    }

    void write_node(uint64_t offset, const Node& node) {
        std::fseek(file_, static_cast<long>(offset), SEEK_SET);
        std::fwrite(&node, sizeof(Node), 1, file_);
    }

    Node read_node(uint64_t offset) {
        Node node{};
        std::fseek(file_, static_cast<long>(offset), SEEK_SET);
        std::fread(&node, sizeof(Node), 1, file_);
        return node;
    }

    uint64_t find_key_offset(uint64_t bucket, const string& key) {
        uint64_t current = bucket_heads_[bucket];
        while (current != 0) {
            Node node = read_node(current);
            if (node.type == 1 && node.len == key.size() && std::memcmp(node.key, key.data(), key.size()) == 0) {
                return current;
            }
            current = node.next;
        }
        return 0;
    }
};

}  // namespace

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    int n = 0;
    if (!(cin >> n)) {
        return 0;
    }

    Storage storage;
    string command;
    string key;
    for (int i = 0; i < n; ++i) {
        cin >> command;
        if (command == "insert") {
            int value;
            cin >> key >> value;
            storage.insert(key, value);
        } else if (command == "delete") {
            int value;
            cin >> key >> value;
            storage.erase(key, value);
        } else if (command == "find") {
            cin >> key;
            vector<int> result = storage.find(key);
            if (result.empty()) {
                cout << "null\n";
            } else {
                for (size_t j = 0; j < result.size(); ++j) {
                    if (j) {
                        cout << ' ';
                    }
                    cout << result[j];
                }
                cout << '\n';
            }
        }
    }

    return 0;
}
