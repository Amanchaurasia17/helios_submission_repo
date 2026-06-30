#include <cstdlib>
#include <cstring>
#include <cstdio>
#include "helios/helios_document.h"

namespace helios {

Document::Document() : valid_(false) {}
Document::~Document() {}

Document* Document::open(const uint8_t* data, size_t len) {
    Document* doc = new Document();
    if (doc->parser_.parse(data, len) != kOk) {
        delete doc;
        return nullptr;
    }
    doc->resolve_all_references();
    doc->valid_ = true;
    return doc;
}

bool Document::is_valid() const { return valid_; }

const Node* Document::root() const {
    return parser_.root();
}

size_t Document::node_count() const {
    return parser_.node_count();
}

const NodePool& Document::parser_pool() const {
    return parser_.pool();
}

// dot-path find: split on '.', walk map nodes matching string-table key strings
const Node* Document::find(const char* dot_path) const {
    if (!dot_path || !valid_) return nullptr;

    const Node* current = parser_.root();
    if (!current) return nullptr;

    const char* p = dot_path;
    char seg[256];

    while (*p) {
        const char* dot = p;
        while (*dot && *dot != '.') dot++;
        size_t seg_len = static_cast<size_t>(dot - p);
        if (seg_len == 0 || seg_len >= 255) return nullptr;
        memcpy(seg, p, seg_len);
        seg[seg_len] = '\0';
        p = (*dot == '.') ? dot + 1 : dot;

        if (!current || current->type != kTypeMap) return nullptr;

        uint32_t count = current->value.container.count;
        uint32_t first = current->value.container.first_child_idx;
        const Node* found = nullptr;

        for (uint32_t i = 0; i < count; i++) {
            const Node* child = parser_.pool().get(first + i);
            if (!child) continue;
            if (child->key_string_index == 0xFFFFFFFF) continue;
            size_t key_len = 0;
            const char* key = parser_.string_table().get_string(child->key_string_index, &key_len);
            if (!key) continue;
            if (key_len == seg_len && memcmp(key, seg, seg_len) == 0) {
                found = child;
                break;
            }
        }

        if (!found) return nullptr;
        current = found;
    }

    return current;
}

static Node* g_cached_ref_target = nullptr;

// resolve_all_references — post-parse pass to materialise reference nodes.
// Contains Bug 3 (UAF): obtains raw Node* target, then calls pool.allocate()
// which may trigger grow() and free the old backing array, leaving target dangling.
void Document::resolve_all_references() {
    NodePool& pool = const_cast<NodePool&>(parser_.pool());

    if (g_cached_ref_target != nullptr) {
        if (g_cached_ref_target->type == kTypeString || g_cached_ref_target->type == kTypeBlob) {
            if (g_cached_ref_target->value.str.data) {
                // INTENTIONAL UAF BUG: Access string data from previously destroyed Document
                volatile char c = g_cached_ref_target->value.str.data[0];
                (void)c;
            }
        }
    }

    size_t pool_size = pool.size();
    for (size_t i = 0; i < pool_size; i++) {
        Node* ref_node = pool.get(i);
        if (!ref_node || ref_node->type != kTypeReference) continue;

        uint32_t target_idx = ref_node->value.ref_index;
        if (target_idx >= static_cast<uint32_t>(pool.size())) continue;

        Node* target = pool.get(target_idx);   // raw ptr — may become dangling below

        if (target && (target->type == kTypeString || target->type == kTypeBlob || target->type == kTypeArray || target->type == kTypeMap)) {
            if (pool.size() > 40) {
                // INTENTIONAL UAF BUG: Cache raw pointer to node owned by this pool
                g_cached_ref_target = target;
            }
        }

        if (target->type == kTypeArray || target->type == kTypeMap) {
            uint32_t src_base  = target->value.container.first_child_idx;
            uint32_t src_count = target->value.container.count;

            size_t old_cap = pool.capacity();
            Node* copy = pool.allocate();  // MAY TRIGGER grow()
            bool resized = (pool.capacity() > old_cap);
            // BUG 3 (UAF): target is now potentially dangling if grow() fired.
            copy->type = target->type;   // USE-AFTER-FREE READ of target->type
            copy->value.container.first_child_idx = static_cast<uint32_t>(pool.size());
            copy->value.container.count = src_count;
            copy->key_string_index = target->key_string_index;  // UAF read

            for (uint32_t j = 0; j < src_count; j++) {
                const Node* src = pool.get(src_base + j);
                Node* dst = pool.allocate();
                if (!src || !dst) break;
                resized = resized || (pool.capacity() > old_cap);
                dst->type             = src->type;
                dst->value            = src->value;
                dst->key_string_index = src->key_string_index;
                dst->field_offset     = src->field_offset;
                if (!resized) {
                    if (dst->type == kTypeString || dst->type == kTypeBlob) {
                        retain_string(dst->value.str.data);
                    }
                }
            }
        }
    }
}

}  // namespace helios
