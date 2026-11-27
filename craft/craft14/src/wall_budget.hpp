#pragma once
#include <csignal>

namespace craft {

inline constexpr char pending_result_name[]=".craft-result-table-v5.partial";
inline volatile std::sig_atomic_t named_result_pending=0;
}
