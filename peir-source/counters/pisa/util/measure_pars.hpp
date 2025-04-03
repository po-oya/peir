#pragma once

#include <iostream>
#include <optional>
#include <string>
#include <map>

#include "query.hpp"


struct block_max_score_query_stat_logging{
    std::string qid;
    size_t term_cnt;
    long while_cnt;

    int f1_cnt_total{0};
    int f2_cnt_total{0};
    int f3_cnt_total{0};
    int p1_cnt_total{0};
    int p2_cnt_total{0};
    int p3_cnt_total{0};
    int p4_cnt_total{0};
    int p5_cnt_total{0};
    int p6_cnt_total{0};
    int p7_cnt_total{0};
    int br1_cnt_total{0};
    int br2_cnt_total{0};

    uint64_t oc_size{0};
    int non_ess_val{0};

    double alg_prep_ms{0};
    double alg_while_ms{0};
    double alg_total_ms{0};

    block_max_score_query_stat_logging(std::string id, size_t tc, size_t wc)
    : qid(id), term_cnt(tc), while_cnt(wc) {

    }

    block_max_score_query_stat_logging() {

    }
};

extern std::map<std::string, block_max_score_query_stat_logging> query_stat_logging;
