#include "storage/avl_tree.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

#include "test_harness.h"

using gredis::AvlTree;

namespace {
using Tree = AvlTree<int>;
using Node = Tree::Node;

// rotations can move the root, so walk parent pointers to find it fresh
Node* root_of(Node* any) {
    while (any->parent != nullptr) {
        any = any->parent;
    }
    return any;
}
} // namespace

TEST(empty_tree) {
    Tree t;
    CHECK(t.empty());
    CHECK_EQ(t.size(), size_t{0});
    CHECK(t.find(1) == nullptr);
    CHECK(t.lower_bound(1) == nullptr);
    CHECK(t.select(0) == nullptr);
    CHECK(t.check_invariants());
}

TEST(insert_rejects_duplicates) {
    Tree t;
    Node* first = t.insert(5);
    CHECK(first != nullptr);
    Node* second = t.insert(5);
    CHECK(second == nullptr);
    CHECK_EQ(t.size(), size_t{1});
}

TEST(find_and_lower_bound_basic) {
    Tree t;
    for (int k : {10, 20, 30}) {
        t.insert(k);
    }
    CHECK(t.find(20) != nullptr);
    CHECK(t.find(15) == nullptr);
    CHECK_EQ(t.lower_bound(15)->key, 20);
    CHECK_EQ(t.lower_bound(10)->key, 10);
    CHECK(t.lower_bound(31) == nullptr);
}

// LL/RR/LR/RL all end up in the same 3-node shape (root 2, left 1, right 3)

TEST(rotation_ll_single_right_rotation) {
    Tree t;
    t.insert(3);
    t.insert(2);
    t.insert(1); // unbalances 3 on its left-left side
    CHECK(t.check_invariants());
    Node* root = root_of(t.find(2));
    CHECK_EQ(root->key, 2);
    CHECK(root->left != nullptr);
    CHECK_EQ(root->left->key, 1);
    CHECK(root->right != nullptr);
    CHECK_EQ(root->right->key, 3);
    CHECK_EQ(root->height, 2);
    CHECK_EQ(root->left->height, 1);
    CHECK_EQ(root->right->height, 1);
    CHECK_EQ(t.size(), size_t{3});
}

TEST(rotation_rr_single_left_rotation) {
    Tree t;
    t.insert(1);
    t.insert(2);
    t.insert(3); // unbalances 1 on its right-right side
    CHECK(t.check_invariants());
    Node* root = root_of(t.find(2));
    CHECK_EQ(root->key, 2);
    CHECK_EQ(root->left->key, 1);
    CHECK_EQ(root->right->key, 3);
    CHECK_EQ(root->height, 2);
}

TEST(rotation_lr_double_rotation) {
    Tree t;
    t.insert(3);
    t.insert(1);
    t.insert(2); // 3's left subtree (rooted at 1) becomes right-heavy
    CHECK(t.check_invariants());
    Node* root = root_of(t.find(2));
    CHECK_EQ(root->key, 2);
    CHECK_EQ(root->left->key, 1);
    CHECK_EQ(root->right->key, 3);
    CHECK_EQ(root->height, 2);
}

TEST(rotation_rl_double_rotation) {
    Tree t;
    t.insert(1);
    t.insert(3);
    t.insert(2); // 1's right subtree (rooted at 3) becomes left-heavy
    CHECK(t.check_invariants());
    Node* root = root_of(t.find(2));
    CHECK_EQ(root->key, 2);
    CHECK_EQ(root->left->key, 1);
    CHECK_EQ(root->right->key, 3);
    CHECK_EQ(root->height, 2);
}

TEST(erase_leaf) {
    Tree t;
    t.insert(2);
    t.insert(1);
    t.insert(3);
    t.erase(t.find(1));
    CHECK(t.check_invariants());
    CHECK_EQ(t.size(), size_t{2});
    CHECK(t.find(1) == nullptr);
    CHECK(t.find(2) != nullptr);
    CHECK(t.find(3) != nullptr);
}

TEST(erase_node_with_one_child) {
    Tree t;
    for (int k : {2, 1, 3, 0}) {
        t.insert(k);
    }
    Node* one = t.find(1);
    CHECK(one->left != nullptr);  // 0
    CHECK(one->right == nullptr); // no right child
    t.erase(one);
    CHECK(t.check_invariants());
    CHECK_EQ(t.size(), size_t{3});
    CHECK(t.find(1) == nullptr);
    CHECK(t.find(0) != nullptr);
}

TEST(erase_root_with_one_child) {
    Tree t;
    t.insert(1);
    t.insert(2);
    Node* root = root_of(t.find(1));
    CHECK_EQ(root->key, 1);
    t.erase(root);
    CHECK(t.check_invariants());
    CHECK_EQ(t.size(), size_t{1});
    Node* new_root = root_of(t.find(2));
    CHECK_EQ(new_root->key, 2);
    CHECK(new_root->parent == nullptr);
}

TEST(erase_root_with_two_children) {
    Tree t;
    t.insert(2);
    t.insert(1);
    t.insert(3);
    t.erase(root_of(t.find(2)));
    CHECK(t.check_invariants());
    CHECK_EQ(t.size(), size_t{2});
    CHECK(t.find(2) == nullptr);
    CHECK(t.find(1) != nullptr);
    CHECK(t.find(3) != nullptr);
}

// erase must relink the successor node itself, not copy its key into the
// erased node and delete the successor -- so a pointer held before the
// erase has to stay valid after
TEST(erase_with_two_children_relinks_successor_not_copies_its_key) {
    Tree t;
    t.insert(2);
    t.insert(1);
    t.insert(3);
    Node* root = root_of(t.find(2));
    Node* successor = t.find(3); // in-order successor of 2
    t.erase(root);
    CHECK(t.check_invariants());
    CHECK_EQ(successor->key, 3);
    CHECK(successor->parent == nullptr);
    CHECK_EQ(successor->left->key, 1);
}

TEST(pointer_stability_across_many_erasures) {
    Tree t;
    constexpr int kN = 5000;
    std::vector<int> keys(kN);
    std::iota(keys.begin(), keys.end(), 0);
    std::mt19937 rng(777);
    std::shuffle(keys.begin(), keys.end(), rng);

    std::unordered_map<int, Node*> node_of;
    node_of.reserve(kN);
    for (int k : keys) {
        node_of[k] = t.insert(k);
    }

    // hold every 5th key, erase the rest
    std::vector<int> held_keys;
    for (int k = 0; k < kN; k += 5) {
        held_keys.push_back(k);
    }
    CHECK_EQ(held_keys.size(), size_t{1000});

    for (int k = 0; k < kN; ++k) {
        if (k % 5 != 0) {
            t.erase(node_of[k]);
        }
    }

    CHECK(t.check_invariants());
    CHECK_EQ(t.size(), held_keys.size());
    for (int k : held_keys) {
        CHECK_EQ(node_of[k]->key, k);
    }
}

TEST(differential_against_std_set_200k_ops) {
    Tree t;
    std::set<int> model;
    std::unordered_map<int, Node*> node_of;

    std::mt19937 rng(20260924);
    std::uniform_int_distribution<int> key_dist(0, 49999);
    std::uniform_int_distribution<int> op_dist(0, 99); // 0-59 insert, 60-99 erase

    constexpr int kOps = 200'000;
    for (int i = 0; i < kOps; ++i) {
        const int key = key_dist(rng);
        const bool do_insert = op_dist(rng) < 60 || model.empty();

        if (do_insert) {
            const auto [it, inserted] = model.insert(key);
            (void)it;
            Node* n = t.insert(key);
            CHECK_EQ(inserted, n != nullptr);
            if (inserted) {
                node_of[key] = n;
            }
        } else {
            // erase a random existing key, not just recently-inserted ones
            auto it = model.begin();
            std::advance(it, std::uniform_int_distribution<size_t>(0, model.size() - 1)(rng));
            const int victim = *it;
            model.erase(it);
            t.erase(node_of[victim]);
            node_of.erase(victim);
        }

        CHECK_EQ(t.size(), model.size());

        if (i % 1000 == 0) {
            CHECK(t.check_invariants());

            const std::vector<int> sorted_model(model.begin(), model.end());
            for (size_t r = 0; r < sorted_model.size(); ++r) {
                Node* selected = t.select(r);
                CHECK(selected != nullptr);
                CHECK_EQ(selected->key, sorted_model[r]);
                CHECK_EQ(t.rank(selected), r);
            }
            CHECK(t.select(sorted_model.size()) == nullptr);

            for (int q : {-1, 0, 25000, 49999, 50000}) {
                Node* lb = t.lower_bound(q);
                auto model_lb = model.lower_bound(q);
                if (model_lb == model.end()) {
                    CHECK(lb == nullptr);
                } else {
                    CHECK(lb != nullptr);
                    CHECK_EQ(lb->key, *model_lb);
                }
            }
        }
    }

    CHECK(t.check_invariants());
    CHECK_EQ(t.size(), model.size());
}

// sequential inserts are the worst case for a plain BST (degenerates to a
// list) -- AVL rebalancing must keep height flat anyway
TEST(height_bound_after_1m_sequential_inserts) {
    Tree t;
    constexpr int kN = 1'000'000;
    for (int i = 0; i < kN; ++i) {
        CHECK(t.insert(i) != nullptr);
    }
    CHECK_EQ(t.size(), static_cast<size_t>(kN));

    Node* root = root_of(t.find(0));
    const double bound = 1.45 * std::log2(static_cast<double>(kN) + 2);
    CHECK(static_cast<double>(root->height) <= bound);
}

TEST_MAIN()
