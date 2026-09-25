#include "storage/zset.h"

#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "test_harness.h"

using gredis::ZSet;

TEST(empty_zset) {
    ZSet z;
    CHECK(z.empty());
    CHECK_EQ(z.size(), size_t{0});
    CHECK(!z.score_of("a").has_value());
    CHECK(!z.rank("a").has_value());
    CHECK(!z.erase("a"));
    CHECK(z.check_invariants());
}

TEST(upsert_new_member_reports_true) {
    ZSet z;
    const auto [score, is_new] = z.upsert("a", 1.5);
    CHECK_EQ(score, 1.5);
    CHECK(is_new);
    CHECK_EQ(z.size(), size_t{1});
    const auto score_a = z.score_of("a");
    CHECK_EQ(*score_a, 1.5);
    CHECK(z.check_invariants());
}

TEST(upsert_existing_member_same_score_reports_false) {
    ZSet z;
    z.upsert("a", 1.5);
    const auto [score, is_new] = z.upsert("a", 1.5);
    CHECK_EQ(score, 1.5);
    CHECK(!is_new);
    CHECK_EQ(z.size(), size_t{1});
}

TEST(upsert_existing_member_changed_score_reports_false_but_updates) {
    ZSet z;
    z.upsert("a", 1.5);
    const auto [score, is_new] = z.upsert("a", 9.0);
    CHECK_EQ(score, 9.0);
    CHECK(!is_new); // "false" here means "not newly added"; the score still changed
    CHECK_EQ(z.size(), size_t{1});
    const auto score_a = z.score_of("a");
    CHECK_EQ(*score_a, 9.0);
    CHECK(z.check_invariants());
}

TEST(erase_removes_member) {
    ZSet z;
    z.upsert("a", 1.0);
    z.upsert("b", 2.0);
    CHECK(z.erase("a"));
    CHECK(!z.erase("a")); // already gone
    CHECK_EQ(z.size(), size_t{1});
    CHECK(!z.score_of("a").has_value());
    CHECK(z.score_of("b").has_value());
    CHECK(z.check_invariants());
}

// equal scores order by member bytes, rank() reflects that
TEST(equal_scores_ordered_by_member) {
    ZSet z;
    z.upsert("charlie", 1.0);
    z.upsert("alpha", 1.0);
    z.upsert("bravo", 1.0);
    const auto rank_alpha = z.rank("alpha");
    const auto rank_bravo = z.rank("bravo");
    const auto rank_charlie = z.rank("charlie");
    CHECK_EQ(*rank_alpha, size_t{0});
    CHECK_EQ(*rank_bravo, size_t{1});
    CHECK_EQ(*rank_charlie, size_t{2});
    CHECK(z.check_invariants());
}

// Re-scoring a member changes its rank without changing the member
// count -- exercises ZSet::upsert's erase-then-reinsert path.
TEST(rescoring_changes_rank_not_count) {
    ZSet z;
    z.upsert("a", 1.0);
    z.upsert("b", 2.0);
    z.upsert("c", 3.0);
    const auto rank_a_before = z.rank("a");
    CHECK_EQ(*rank_a_before, size_t{0});
    z.upsert("a", 5.0); // now the largest
    CHECK_EQ(z.size(), size_t{3});
    const auto rank_a_after = z.rank("a");
    const auto rank_b = z.rank("b");
    const auto rank_c = z.rank("c");
    CHECK_EQ(*rank_a_after, size_t{2});
    CHECK_EQ(*rank_b, size_t{0});
    CHECK_EQ(*rank_c, size_t{1});
    CHECK(z.check_invariants());
}

namespace {

// mirrors ZSet::EntryLess so the model's sort order matches exactly
bool entry_less(const std::pair<double, std::string>& a, const std::pair<double, std::string>& b) {
    if (a.first != b.first) {
        return a.first < b.first;
    }
    return a.second < b.second;
}

} // namespace

TEST(differential_against_std_map_200k_ops) {
    ZSet z;
    std::map<std::string, double> model;

    std::mt19937 rng(20260925);
    std::uniform_int_distribution<int> key_dist(0, 4999);
    std::uniform_real_distribution<double> score_dist(-1000.0, 1000.0);
    std::uniform_int_distribution<int> op_dist(0, 99); // 0-69 upsert, 70-99 erase

    constexpr int kOps = 200'000;
    for (int i = 0; i < kOps; ++i) {
        const std::string key = "m" + std::to_string(key_dist(rng));
        const bool do_upsert = op_dist(rng) < 70 || model.empty();

        if (do_upsert) {
            const double score = score_dist(rng);
            const auto [applied, is_new] = z.upsert(key, score);
            CHECK_EQ(applied, score);
            const auto model_it = model.find(key);
            CHECK_EQ(is_new, model_it == model.end());
            model[key] = score;
        } else {
            auto it = model.begin();
            std::advance(it, std::uniform_int_distribution<size_t>(0, model.size() - 1)(rng));
            const std::string victim = it->first;
            model.erase(it);
            CHECK(z.erase(victim));
        }

        CHECK_EQ(z.size(), model.size());

        if (i % 1000 == 0) {
            CHECK(z.check_invariants());

            std::vector<std::pair<double, std::string>> sorted_model;
            sorted_model.reserve(model.size());
            for (const auto& [member, score] : model) {
                sorted_model.emplace_back(score, member);
            }
            std::sort(sorted_model.begin(), sorted_model.end(), entry_less);

            for (size_t r = 0; r < sorted_model.size(); ++r) {
                const auto r_rank = z.rank(sorted_model[r].second);
                const auto r_score = z.score_of(sorted_model[r].second);
                CHECK_EQ(*r_rank, r);
                CHECK_EQ(*r_score, sorted_model[r].first);
            }
        }
    }

    CHECK(z.check_invariants());
    CHECK_EQ(z.size(), model.size());
}

TEST_MAIN()
