#include "data_ids.hpp"

inline constexpr uint8_t set_size = 8;
struct commodity_set {
                                dcon::commodity_id commodity[set_size];
                                int amount[set_size];
};
struct item_set {
                                dcon::item_type_id item[set_size];
};
struct skill_set {
                                dcon::skill_id skill[set_size];
                                float value[set_size];
};