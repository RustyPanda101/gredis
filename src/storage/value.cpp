#include "storage/value.h"

#include <type_traits>
#include <variant>

namespace gredis {

size_t value_element_count(const Value& v) {
    return std::visit(
        [](const auto& alt) -> size_t {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return 1;
            } else {
                return alt->size(); // unique_ptr<HashValue|ListValue|SetValue|ZSet>
            }
        },
        v);
}

const char* type_name(const Value& v) {
    switch (v.index()) {
        case 0:
            return "string";
        case 1:
            return "hash";
        case 2:
            return "list";
        case 3:
            return "set";
        case 4:
            return "zset";
    }
    return "none"; // unreachable, every alternative handled above
}

} // namespace gredis
