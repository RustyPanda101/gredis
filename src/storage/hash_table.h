// hash table with incremental rehash, so a resize doesn't block
// everyone at once like unordered_map's one-shot rehash would
//
// buckets own raw Node* pointers, freed manually here (only place in
// this file that does that)
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

namespace gredis {

// FNV-1a 64-bit. Not DoS-resistant, fine for this project.
struct FnvHash {
    uint64_t operator()(std::string_view s) const noexcept {
        uint64_t h = 0xcbf29ce484222325ULL; // FNV offset basis
        for (unsigned char c : s) {
            h ^= c;
            h *= 0x100000001b3ULL; // FNV prime
        }
        return h;
    }
};

// lets find(string_view) work on a HashTable<std::string, V> without
// materializing a temporary std::string
struct TransparentEqual {
    using is_transparent = void;
    template <typename A, typename B>
    bool operator()(const A& a, const B& b) const {
        return a == b;
    }
};

template <typename K, typename V, typename Hash = FnvHash, typename Eq = TransparentEqual>
class HashTable {
public:
    HashTable() { reset_to_empty(); }

    ~HashTable() {
        free_table(old_table_);
        free_table(new_table_);
    }

    HashTable(const HashTable&) = delete;
    HashTable& operator=(const HashTable&) = delete;

    HashTable(HashTable&& other) noexcept
        : old_table_(std::move(other.old_table_)),
          new_table_(std::move(other.new_table_)),
          rehash_idx_(other.rehash_idx_) {
        other.reset_to_empty();
    }

    HashTable& operator=(HashTable&& other) noexcept {
        if (this != &other) {
            free_table(old_table_);
            free_table(new_table_);
            old_table_ = std::move(other.old_table_);
            new_table_ = std::move(other.new_table_);
            rehash_idx_ = other.rehash_idx_;
            other.reset_to_empty();
        }
        return *this;
    }

    // non-const find also ticks the rehash forward by one bucket, const
    // version can't mutate state so it skips that
    template <typename KeyLike>
    V* find(const KeyLike& key) {
        maybe_rehash_step();
        const uint64_t h = Hash{}(key);
        if (Node* n = find_in(old_table_, key, h)) {
            return &n->value;
        }
        if (is_rehashing()) {
            if (Node* n = find_in(new_table_, key, h)) {
                return &n->value;
            }
        }
        return nullptr;
    }

    template <typename KeyLike>
    const V* find(const KeyLike& key) const {
        const uint64_t h = Hash{}(key);
        if (const Node* n = find_in(old_table_, key, h)) {
            return &n->value;
        }
        if (is_rehashing()) {
            if (const Node* n = find_in(new_table_, key, h)) {
                return &n->value;
            }
        }
        return nullptr;
    }

    // returns pointer to value + whether it was newly inserted
    std::pair<V*, bool> insert_or_assign(K key, V value) {
        maybe_rehash_step();

        const uint64_t h = Hash{}(key);
        if (Node* existing = find_in(old_table_, key, h)) {
            existing->value = std::move(value);
            return {&existing->value, false};
        }
        if (is_rehashing()) {
            if (Node* existing = find_in(new_table_, key, h)) {
                existing->value = std::move(value);
                return {&existing->value, false};
            }
        }

        // new key pushing load factor >= 1.0 kicks off a rehash
        if (!is_rehashing() && old_table_.count + 1 >= old_table_.buckets.size()) {
            start_rehash(old_table_.buckets.size() * 2);
        }

        Table& dest = is_rehashing() ? new_table_ : old_table_;
        auto* node = new Node(std::move(key), std::move(value), h);
        const size_t idx = h & (dest.buckets.size() - 1);
        node->next = dest.buckets[idx];
        dest.buckets[idx] = node;
        ++dest.count;
        return {&node->value, true};
    }

    template <typename KeyLike>
    bool erase(const KeyLike& key) {
        maybe_rehash_step();
        const uint64_t h = Hash{}(key);
        Node* n = unlink_from(old_table_, key, h);
        if (!n && is_rehashing()) {
            n = unlink_from(new_table_, key, h);
        }
        if (!n) {
            return false;
        }
        delete n;
        maybe_start_shrink();
        return true;
    }

    // moves the value out instead of destroying it in place, so UNLINK
    // can hand it off to the thread pool instead of freeing inline
    template <typename KeyLike>
    std::optional<V> take(const KeyLike& key) {
        maybe_rehash_step();
        const uint64_t h = Hash{}(key);
        Node* n = unlink_from(old_table_, key, h);
        if (!n && is_rehashing()) {
            n = unlink_from(new_table_, key, h);
        }
        if (!n) {
            return std::nullopt;
        }
        std::optional<V> result(std::move(n->value));
        delete n;
        maybe_start_shrink();
        return result;
    }

    size_t size() const { return old_table_.count + new_table_.count; }
    bool empty() const { return size() == 0; }

    void clear() {
        free_table(old_table_);
        free_table(new_table_);
        reset_to_empty();
    }

    size_t bucket_count() const {
        return old_table_.buckets.size() + (is_rehashing() ? new_table_.buckets.size() : 0);
    }
    bool is_rehashing() const { return rehash_idx_ >= 0; }

    // don't insert/erase while iterating
    template <typename Fn>
    void for_each(Fn&& fn) {
        for (Node* head : old_table_.buckets) {
            for (Node* n = head; n != nullptr; n = n->next) {
                fn(static_cast<const K&>(n->key), n->value);
            }
        }
        if (is_rehashing()) {
            for (Node* head : new_table_.buckets) {
                for (Node* n = head; n != nullptr; n = n->next) {
                    fn(static_cast<const K&>(n->key), n->value);
                }
            }
        }
    }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (Node* head : old_table_.buckets) {
            for (const Node* n = head; n != nullptr; n = n->next) {
                fn(n->key, n->value);
            }
        }
        if (is_rehashing()) {
            for (Node* head : new_table_.buckets) {
                for (const Node* n = head; n != nullptr; n = n->next) {
                    fn(n->key, n->value);
                }
            }
        }
    }

    // approximately (not perfectly) uniform random live entry, or
    // {nullptr, nullptr} if empty -- biased toward short chains
    template <typename Rng>
    std::pair<const K*, V*> random_entry(Rng& rng) {
        if (size() == 0) {
            return {nullptr, nullptr};
        }

        Table* t = &old_table_;
        if (is_rehashing()) {
            std::uniform_int_distribution<size_t> pick_table(0, size() - 1);
            t = (pick_table(rng) < old_table_.count) ? &old_table_ : &new_table_;
            if (t->count == 0) {
                // rare: chosen table happened to be empty, fall back to the other
                t = (t == &old_table_) ? &new_table_ : &old_table_;
            }
        }
        if (t->buckets.empty() || t->count == 0) {
            return {nullptr, nullptr};
        }

        std::uniform_int_distribution<size_t> pick_bucket(0, t->buckets.size() - 1);
        Node* head = nullptr;
        do {
            head = t->buckets[pick_bucket(rng)];
        } while (head == nullptr);

        size_t chain_len = 0;
        for (Node* n = head; n != nullptr; n = n->next) {
            ++chain_len;
        }
        std::uniform_int_distribution<size_t> pick_pos(0, chain_len - 1);
        size_t steps = pick_pos(rng);
        Node* n = head;
        while (steps-- > 0) {
            n = n->next;
        }
        return {&n->key, &n->value};
    }

    // migrates up to `budget` non-empty buckets (skipping at most
    // 10*budget empty ones). no-op if not rehashing. also called
    // directly from the cron tick so an idle table still finishes.
    void rehash_step(size_t budget) {
        if (!is_rehashing() || budget == 0) {
            return;
        }

        size_t migrated = 0;
        size_t empty_skipped = 0;
        const size_t empty_budget = 10 * budget;

        while (migrated < budget && empty_skipped < empty_budget &&
               static_cast<size_t>(rehash_idx_) < old_table_.buckets.size()) {
            const size_t idx = static_cast<size_t>(rehash_idx_);
            Node* head = old_table_.buckets[idx];
            if (head == nullptr) {
                ++rehash_idx_;
                ++empty_skipped;
                continue;
            }

            size_t moved = 0;
            Node* n = head;
            while (n != nullptr) {
                Node* next = n->next;
                const size_t new_idx = n->hash & (new_table_.buckets.size() - 1);
                n->next = new_table_.buckets[new_idx];
                new_table_.buckets[new_idx] = n;
                n = next;
                ++moved;
            }
            old_table_.buckets[idx] = nullptr;
            old_table_.count -= moved;
            new_table_.count += moved;

            ++rehash_idx_;
            ++migrated;
        }

        if (static_cast<size_t>(rehash_idx_) >= old_table_.buckets.size()) {
            // done, old table's empty now -- swap in the new one
            old_table_ = std::move(new_table_);
            new_table_ = Table{};
            rehash_idx_ = -1;
        }
    }

    // test-only: checks node counts, correct buckets, no dup keys, and
    // that old buckets below the rehash cursor are empty
    bool check_invariants() const {
        if (!is_rehashing()) {
            if (!new_table_.buckets.empty() || new_table_.count != 0) {
                return false;
            }
        } else {
            const size_t limit = std::min(static_cast<size_t>(rehash_idx_), old_table_.buckets.size());
            for (size_t i = 0; i < limit; ++i) {
                if (old_table_.buckets[i] != nullptr) {
                    return false;
                }
            }
        }

        std::vector<const Node*> all_nodes;

        size_t counted_old = 0;
        for (size_t i = 0; i < old_table_.buckets.size(); ++i) {
            for (const Node* n = old_table_.buckets[i]; n != nullptr; n = n->next) {
                if ((n->hash & (old_table_.buckets.size() - 1)) != i) {
                    return false;
                }
                all_nodes.push_back(n);
                ++counted_old;
            }
        }
        if (counted_old != old_table_.count) {
            return false;
        }

        if (is_rehashing()) {
            size_t counted_new = 0;
            for (size_t i = 0; i < new_table_.buckets.size(); ++i) {
                for (const Node* n = new_table_.buckets[i]; n != nullptr; n = n->next) {
                    if ((n->hash & (new_table_.buckets.size() - 1)) != i) {
                        return false;
                    }
                    all_nodes.push_back(n);
                    ++counted_new;
                }
            }
            if (counted_new != new_table_.count) {
                return false;
            }
        }

        // dup-key check: sort by hash, then compare within each run of equal hashes
        std::sort(all_nodes.begin(), all_nodes.end(),
                  [](const Node* a, const Node* b) { return a->hash < b->hash; });
        for (size_t i = 0; i + 1 < all_nodes.size();) {
            size_t j = i + 1;
            while (j < all_nodes.size() && all_nodes[j]->hash == all_nodes[i]->hash) {
                ++j;
            }
            for (size_t a = i; a < j; ++a) {
                for (size_t b = a + 1; b < j; ++b) {
                    if (Eq{}(all_nodes[a]->key, all_nodes[b]->key)) {
                        return false;
                    }
                }
            }
            i = j;
        }

        return true;
    }

private:
    struct Node {
        K key;
        V value;
        uint64_t hash;
        Node* next; // owning pointer to the next node in this bucket's chain

        Node(K k, V v, uint64_t h) : key(std::move(k)), value(std::move(v)), hash(h), next(nullptr) {}
    };

    struct Table {
        std::vector<Node*> buckets; // each slot owns the head of a chain
        size_t count = 0;
    };

    static constexpr size_t kInitialBuckets = 4;

    template <typename KeyLike>
    static Node* find_in(Table& t, const KeyLike& key, uint64_t h) {
        if (t.buckets.empty()) {
            return nullptr;
        }
        for (Node* n = t.buckets[h & (t.buckets.size() - 1)]; n != nullptr; n = n->next) {
            if (n->hash == h && Eq{}(n->key, key)) {
                return n;
            }
        }
        return nullptr;
    }

    template <typename KeyLike>
    static const Node* find_in(const Table& t, const KeyLike& key, uint64_t h) {
        if (t.buckets.empty()) {
            return nullptr;
        }
        for (const Node* n = t.buckets[h & (t.buckets.size() - 1)]; n != nullptr; n = n->next) {
            if (n->hash == h && Eq{}(n->key, key)) {
                return n;
            }
        }
        return nullptr;
    }

    template <typename KeyLike>
    static Node* unlink_from(Table& t, const KeyLike& key, uint64_t h) {
        if (t.buckets.empty()) {
            return nullptr;
        }
        Node** link = &t.buckets[h & (t.buckets.size() - 1)];
        while (*link != nullptr) {
            Node* n = *link;
            if (n->hash == h && Eq{}(n->key, key)) {
                *link = n->next;
                --t.count;
                return n;
            }
            link = &n->next;
        }
        return nullptr;
    }

    void maybe_rehash_step() {
        if (is_rehashing()) {
            rehash_step(1);
        }
    }

    void start_rehash(size_t new_bucket_count) {
        new_bucket_count = std::max(new_bucket_count, kInitialBuckets);
        new_table_.buckets.assign(new_bucket_count, nullptr);
        new_table_.count = 0;
        rehash_idx_ = 0;
    }

    void maybe_start_shrink() {
        if (is_rehashing()) {
            return;
        }
        if (old_table_.buckets.size() <= kInitialBuckets) {
            return;
        }
        // Shrink when load < 0.1 and buckets > 4 (halve).
        if (old_table_.count * 10 < old_table_.buckets.size()) {
            size_t target = old_table_.buckets.size() / 2;
            target = std::max(target, kInitialBuckets);
            if (target < old_table_.buckets.size()) {
                start_rehash(target);
            }
        }
    }

    static void free_table(Table& t) {
        for (Node* head : t.buckets) {
            Node* n = head;
            while (n != nullptr) {
                Node* next = n->next;
                delete n;
                n = next;
            }
        }
    }

    void reset_to_empty() {
        old_table_.buckets.assign(kInitialBuckets, nullptr);
        old_table_.count = 0;
        new_table_.buckets.clear();
        new_table_.count = 0;
        rehash_idx_ = -1;
    }

    Table old_table_;
    Table new_table_;
    long long rehash_idx_ = -1; // -1 = not rehashing, else the next old bucket to migrate
};

} // namespace gredis
