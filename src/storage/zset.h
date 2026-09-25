// sorted set: score-ordered AvlTree + a HashTable for O(1) member->node
// lookup. tree_ owns every AvlNode; dict_'s Node* values are
// non-owning pointers into it. every dict entry must point to a live
// node in tree_ and vice versa -- only this class's methods touch both,
// never a command handler directly.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "storage/avl_tree.h"
#include "storage/hash_table.h"

namespace gredis {

class ZSet {
public:
    // ordering key: score ascending, ties broken by member bytes
    // (matches Redis)
    struct Entry {
        double score;
        std::string member;
    };
    struct EntryLess {
        bool operator()(const Entry& a, const Entry& b) const {
            if (a.score != b.score) {
                return a.score < b.score;
            }
            return a.member < b.member;
        }
    };
    using Tree = AvlTree<Entry, EntryLess>;
    using Node = Tree::Node;

    // {score, true} if newly added, {score, false} if member already
    // existed (regardless of whether score changed -- caller can
    // compare against score_of() beforehand if it needs to know, e.g.
    // ZADD CH). score changes go through erase+insert, never mutated
    // in place, since score is part of the tree's ordering key.
    std::pair<double, bool> upsert(std::string member, double score) {
        Node** existing = dict_.find(member);
        if (existing != nullptr) {
            Node* old_node = *existing;
            if (old_node->key.score == score) {
                return {score, false};
            }
            tree_.erase(old_node);
            Node* new_node = tree_.insert(Entry{score, member});
            *existing = new_node;
            return {score, false};
        }
        Node* node = tree_.insert(Entry{score, member}); // tree gets its own copy of member
        dict_.insert_or_assign(std::move(member), node); // dict key takes the original
        return {score, true};
    }

    bool erase(std::string_view member) {
        Node** existing = dict_.find(member);
        if (existing == nullptr) {
            return false;
        }
        tree_.erase(*existing);
        dict_.erase(member);
        return true;
    }

    // returns by value, not pointer -- a mutable score pointer would
    // let a caller corrupt the tree's ordering
    std::optional<double> score_of(std::string_view member) {
        Node** existing = dict_.find(member);
        if (existing == nullptr) {
            return std::nullopt;
        }
        return (*existing)->key.score;
    }

    std::optional<size_t> rank(std::string_view member) {
        Node** existing = dict_.find(member);
        if (existing == nullptr) {
            return std::nullopt;
        }
        return tree_.rank(*existing);
    }

    size_t size() const { return dict_.size(); }
    bool empty() const { return dict_.empty(); }

    // cmd_zset.cpp reads this directly for select()/lower_bound()/next()
    Tree& tree() { return tree_; }

    bool check_invariants() const {
        if (!tree_.check_invariants()) {
            return false;
        }
        if (tree_.size() != dict_.size()) {
            return false;
        }
        bool ok = true;
        dict_.for_each([&ok](const std::string& member, Node* node) {
            if (node == nullptr || node->key.member != member) {
                ok = false;
            }
        });
        return ok;
    }

private:
    HashTable<std::string, Node*> dict_; // non-owning pointers into tree_
    Tree tree_;
};

} // namespace gredis
