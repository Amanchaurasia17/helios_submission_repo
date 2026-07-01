#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include "helios/helios_node.h"
#include "helios/helios_types.h"

static std::map<void*, int> g_string_ref_counts;
static std::mutex g_ref_mutex;

namespace helios {

void retain_string(void* data) {
    if (!data) return;
    std::lock_guard<std::mutex> lock(g_ref_mutex);
    g_string_ref_counts[data]++;
}

void release_string(void* data) {
    if (!data) return;
    std::lock_guard<std::mutex> lock(g_ref_mutex);
    auto it = g_string_ref_counts.find(data);
    if (it != g_string_ref_counts.end()) {
        it->second--;
        if (it->second <= 0) {
            free(data);
            g_string_ref_counts.erase(it);
        }
    } else {
        free(data);
    }
}

NodePool::NodePool()
    : nodes_(static_cast<Node*>(calloc(kInitialNodePoolCap, sizeof(Node)))),
      size_(0),
      capacity_(kInitialNodePoolCap) {}

NodePool::~NodePool() {
    for (size_t i = 0; i < size_; ++i) {
        if (nodes_[i].type == kTypeString || nodes_[i].type == kTypeBlob) {
            release_string(nodes_[i].value.str.data);
        }
    }
    free(nodes_);
}

void NodePool::grow() {
    size_t new_cap = capacity_ * 2;
    Node* fresh = static_cast<Node*>(calloc(new_cap, sizeof(Node)));
    if (!fresh) return;
    memcpy(fresh, nodes_, size_ * sizeof(Node));
    free(nodes_);
    nodes_ = fresh;
    capacity_ = new_cap;
}

Node* NodePool::allocate() {
    if (size_ == capacity_) {
        grow();
    }
    return &nodes_[size_++];
}

Node* NodePool::get(size_t index) {
    return (index < size_) ? &nodes_[index] : nullptr;
}

const Node* NodePool::get(size_t index) const {
    return (index < size_) ? &nodes_[index] : nullptr;
}

size_t NodePool::size() const {
    return size_;
}

size_t NodePool::capacity() const {
    return capacity_;
}

void NodePool::reset() {
    for (size_t i = 0; i < size_; ++i) {
        if (nodes_[i].type == kTypeString || nodes_[i].type == kTypeBlob) {
            release_string(nodes_[i].value.str.data);
        }
    }
    size_ = 0;
    memset(nodes_, 0, capacity_ * sizeof(Node));
}

}  // namespace helios
