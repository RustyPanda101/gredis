// AVL tree with per-node subtree sizes for O(log n) rank/select.
// standalone, doesn't know about ZSet/scores/members.
//
// nodes are raw owning new/delete pointers. destroy/check_invariants
// recurse instead of using an explicit stack -- AVL's height bound
// (~1.44*log2 n) means recursion depth is never actually a risk.
//
// erase() with two children relinks the successor node into the erased
// spot instead of copying its key and deleting the successor -- ZSet
// holds raw AvlNode* pointers into this tree, so erasing N must never
// invalidate a pointer to some other live node.
#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <utility>

namespace gredis {

template <typename Key, typename Compare = std::less<Key>>
class AvlTree {
public:
    struct Node {
        Key key;
        int height = 1;
        size_t size = 1;
        Node* left = nullptr;
        Node* right = nullptr;
        Node* parent = nullptr;

        explicit Node(Key k) : key(std::move(k)) {}
    };

    AvlTree() = default;
    ~AvlTree() { destroy(root_); }

    AvlTree(const AvlTree&) = delete;
    AvlTree& operator=(const AvlTree&) = delete;

    AvlTree(AvlTree&& other) noexcept : root_(other.root_), count_(other.count_) {
        other.root_ = nullptr;
        other.count_ = 0;
    }

    AvlTree& operator=(AvlTree&& other) noexcept {
        if (this != &other) {
            destroy(root_);
            root_ = other.root_;
            count_ = other.count_;
            other.root_ = nullptr;
            other.count_ = 0;
        }
        return *this;
    }

    // returns nullptr without modifying the tree if an equivalent key
    // already exists (duplicates rejected -- ZSet's (score, member)
    // keys are always unique anyway)
    Node* insert(Key key) {
        if (root_ == nullptr) {
            root_ = new Node(std::move(key));
            ++count_;
            return root_;
        }
        Node* cur = root_;
        Node* parent = nullptr;
        bool went_left = false;
        while (cur != nullptr) {
            parent = cur;
            if (less(key, cur->key)) {
                cur = cur->left;
                went_left = true;
            } else if (less(cur->key, key)) {
                cur = cur->right;
                went_left = false;
            } else {
                return nullptr; // equivalent key already present
            }
        }
        Node* node = new Node(std::move(key));
        node->parent = parent;
        if (went_left) {
            parent->left = node;
        } else {
            parent->right = node;
        }
        ++count_;
        rebalance_up(parent);
        return node;
    }

    // n must be owned by this tree. every other live node's Node* stays
    // valid (see the transplant note above).
    void erase(Node* n) {
        Node* fixup_start;

        if (n->left == nullptr) {
            fixup_start = n->parent;
            transplant(n, n->right);
        } else if (n->right == nullptr) {
            fixup_start = n->parent;
            transplant(n, n->left);
        } else {
            Node* s = leftmost(n->right);
            if (s->parent == n) {
                fixup_start = s;
            } else {
                fixup_start = s->parent;
                transplant(s, s->right); // s is leftmost, so it has no left child
                s->right = n->right;
                s->right->parent = s;
            }
            transplant(n, s);
            s->left = n->left;
            s->left->parent = s;
        }

        delete n;
        --count_;
        rebalance_up(fixup_start);
    }

    Node* find(const Key& key) {
        Node* cur = root_;
        while (cur != nullptr) {
            if (less(key, cur->key)) {
                cur = cur->left;
            } else if (less(cur->key, key)) {
                cur = cur->right;
            } else {
                return cur;
            }
        }
        return nullptr;
    }

    // smallest key >= `key`, or nullptr if every key is smaller
    Node* lower_bound(const Key& key) {
        Node* cur = root_;
        Node* result = nullptr;
        while (cur != nullptr) {
            if (!less(cur->key, key)) {
                result = cur;
                cur = cur->left;
            } else {
                cur = cur->right;
            }
        }
        return result;
    }

    // 0-based rank: number of nodes with a smaller key
    size_t rank(const Node* n) const {
        size_t r = subtree_size(n->left);
        for (const Node* cur = n; cur->parent != nullptr; cur = cur->parent) {
            if (cur == cur->parent->right) {
                r += subtree_size(cur->parent->left) + 1;
            }
        }
        return r;
    }

    // node at 0-based `rank` in sorted order, or nullptr if rank >= size()
    Node* select(size_t rank) {
        Node* cur = root_;
        while (cur != nullptr) {
            const size_t left_size = subtree_size(cur->left);
            if (rank < left_size) {
                cur = cur->left;
            } else if (rank == left_size) {
                return cur;
            } else {
                rank -= left_size + 1;
                cur = cur->right;
            }
        }
        return nullptr;
    }

    static Node* next(Node* n) {
        if (n->right != nullptr) {
            return leftmost(n->right);
        }
        Node* cur = n;
        while (cur->parent != nullptr && cur == cur->parent->right) {
            cur = cur->parent;
        }
        return cur->parent;
    }

    static Node* prev(Node* n) {
        if (n->left != nullptr) {
            return rightmost(n->left);
        }
        Node* cur = n;
        while (cur->parent != nullptr && cur == cur->parent->left) {
            cur = cur->parent;
        }
        return cur->parent;
    }

    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }

    // test-only: checks strictly-increasing in-order keys, parent
    // pointers, correct heights/sizes, and |balance factor| <= 1
    bool check_invariants() const {
        if (root_ != nullptr && root_->parent != nullptr) {
            return false;
        }
        const Key* prev_key = nullptr;
        size_t visited = 0;
        if (!check_node(root_, prev_key, visited)) {
            return false;
        }
        return visited == count_;
    }

private:
    static int height_of(const Node* n) { return n != nullptr ? n->height : 0; }
    static size_t subtree_size(const Node* n) { return n != nullptr ? n->size : 0; }
    static int balance_factor(const Node* n) { return height_of(n->left) - height_of(n->right); }

    static void update(Node* n) {
        n->height = 1 + std::max(height_of(n->left), height_of(n->right));
        n->size = 1 + subtree_size(n->left) + subtree_size(n->right);
    }

    static Node* leftmost(Node* n) {
        while (n->left != nullptr) {
            n = n->left;
        }
        return n;
    }

    static Node* rightmost(Node* n) {
        while (n->right != nullptr) {
            n = n->right;
        }
        return n;
    }

    bool less(const Key& a, const Key& b) const { return Compare{}(a, b); }

    // relinks v into u's spot from u->parent's perspective; doesn't
    // touch u's/v's own child pointers, caller handles those
    void transplant(Node* u, Node* v) {
        if (u->parent == nullptr) {
            root_ = v;
        } else if (u == u->parent->left) {
            u->parent->left = v;
        } else {
            u->parent->right = v;
        }
        if (v != nullptr) {
            v->parent = u->parent;
        }
    }

    // x's right child becomes the new subtree root, fully relinked to
    // x's old parent. returns the new subtree root.
    Node* rotate_left(Node* x) {
        Node* y = x->right;
        x->right = y->left;
        if (y->left != nullptr) {
            y->left->parent = x;
        }
        y->parent = x->parent;
        if (x->parent == nullptr) {
            root_ = y;
        } else if (x == x->parent->left) {
            x->parent->left = y;
        } else {
            x->parent->right = y;
        }
        y->left = x;
        x->parent = y;
        update(x); // x's children changed, must update before y
        update(y);
        return y;
    }

    Node* rotate_right(Node* x) {
        Node* y = x->left;
        x->left = y->right;
        if (y->right != nullptr) {
            y->right->parent = x;
        }
        y->parent = x->parent;
        if (x->parent == nullptr) {
            root_ = y;
        } else if (x == x->parent->left) {
            x->parent->left = y;
        } else {
            x->parent->right = y;
        }
        y->right = x;
        x->parent = y;
        update(x);
        update(y);
        return y;
    }

    void rebalance_node(Node* cur) {
        update(cur);
        const int bf = balance_factor(cur);
        if (bf > 1) {
            if (balance_factor(cur->left) < 0) {
                rotate_left(cur->left); // LR case: straighten left child first
            }
            rotate_right(cur);
        } else if (bf < -1) {
            if (balance_factor(cur->right) > 0) {
                rotate_right(cur->right); // RL case
            }
            rotate_left(cur);
        }
    }

    // start may be nullptr (e.g. erasing the tree's only node) -- no-op then
    void rebalance_up(Node* start) {
        for (Node* cur = start; cur != nullptr;) {
            // grab parent before rebalance_node() rotates cur elsewhere --
            // rotations always relink back to this same parent, so it's still safe
            Node* parent = cur->parent;
            rebalance_node(cur);
            cur = parent;
        }
    }

    bool check_node(const Node* n, const Key*& prev_key, size_t& visited) const {
        if (n == nullptr) {
            return true;
        }
        if (!check_node(n->left, prev_key, visited)) {
            return false;
        }
        if (prev_key != nullptr && !less(*prev_key, n->key)) {
            return false; // not strictly increasing -> duplicate or out of order
        }
        prev_key = &n->key;
        ++visited;
        if (n->left != nullptr && n->left->parent != n) {
            return false;
        }
        if (n->right != nullptr && n->right->parent != n) {
            return false;
        }
        if (n->height != 1 + std::max(height_of(n->left), height_of(n->right))) {
            return false;
        }
        const int bf = balance_factor(n);
        if (bf > 1 || bf < -1) {
            return false;
        }
        if (n->size != 1 + subtree_size(n->left) + subtree_size(n->right)) {
            return false;
        }
        return check_node(n->right, prev_key, visited);
    }

    static void destroy(Node* n) {
        if (n == nullptr) {
            return;
        }
        destroy(n->left);
        destroy(n->right);
        delete n;
    }

    Node* root_ = nullptr;
    size_t count_ = 0;
};

} // namespace gredis
