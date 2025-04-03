#pragma once

#include <vector>
#include <string>

#include "concepts.hpp"
#include "concepts/posting_cursor.hpp"
#include "topk_queue.hpp"
#include "util/measure_pars.hpp"

namespace pisa {

struct block_max_maxscore_query {
    /******/
    std::string qid;
    /******/

    explicit block_max_maxscore_query(topk_queue& topk) : m_topk(topk) {}
    explicit block_max_maxscore_query(topk_queue& topk, std::string id) : m_topk(topk), qid(id) {}

    template <typename CursorRange>
    PISA_REQUIRES((concepts::BlockMaxPostingCursor<pisa::val_t<CursorRange>>))
    void operator()(CursorRange&& cursors, uint64_t max_docid) {
        /**********/
        auto start_alg = std::chrono::steady_clock::now();
        block_max_score_query_stat_logging& this_query_stats = query_stat_logging[qid];
        /**********/

        using Cursor = typename std::decay_t<CursorRange>::value_type;
        if (cursors.empty()) {
            return;
        }

        std::vector<Cursor*> ordered_cursors;
        ordered_cursors.reserve(cursors.size());
        for (auto& en: cursors) {
            ordered_cursors.push_back(&en);
        }
 

        // sort enumerators by increasing maxscore
        std::sort(ordered_cursors.begin(), ordered_cursors.end(), [](Cursor* lhs, Cursor* rhs) {
            return lhs->max_score() < rhs->max_score();
        });

        std::vector<float> upper_bounds(ordered_cursors.size());
        upper_bounds[0] = ordered_cursors[0]->max_score();
        for (size_t i = 1; i < ordered_cursors.size(); ++i) {
            upper_bounds[i] = upper_bounds[i - 1] + ordered_cursors[i]->max_score();
        }

        int non_essential_lists = 0;
        uint64_t cur_doc =
            std::min_element(cursors.begin(), cursors.end(), [](Cursor const& lhs, Cursor const& rhs) {
                return lhs.docid() < rhs.docid();
            })->docid();


        /**********/
        // this_query_stats.oc_size = ordered_cursors.size();
        /**********/

        /**********/
        auto start_alg_exec = std::chrono::steady_clock::now();
        /**********/

        while (non_essential_lists < ordered_cursors.size() && cur_doc < max_docid) {
            /*******/
            // w_cnt
            // this_query_stats.while_cnt++;
            /*******/
            
            float score = 0;
            uint64_t next_doc = max_docid;
            for (size_t i = non_essential_lists; i < ordered_cursors.size(); ++i) {
                // f1_cnt
                /**********/
                // this_query_stats.f1_cnt_total++;
                /**********/

                if (ordered_cursors[i]->docid() == cur_doc) {
                    // p1_cnt
                    /*******/
                    // this_query_stats.p1_cnt_total++;
                    /*******/

                    score += ordered_cursors[i]->score();
                    ordered_cursors[i]->next();
                }
                if (ordered_cursors[i]->docid() < next_doc) {
                    // p2_cnt
                    /**********/
                    // this_query_stats.p2_cnt_total++;
                    /**********/

                    next_doc = ordered_cursors[i]->docid();
                }
            }

            double block_upper_bound =
                non_essential_lists > 0 ? upper_bounds[non_essential_lists - 1] : 0;
            for (int i = non_essential_lists - 1; i + 1 > 0; --i) {
                // f2_cnt
                /**********/
                // this_query_stats.f2_cnt_total++;
                /**********/

                if (ordered_cursors[i]->block_max_docid() < cur_doc) {
                    // p3_cnt
                    /*******/
                    // query_stat_logging[qid].p3_cnt_total++;
                    /*******/
                    ordered_cursors[i]->block_max_next_geq(cur_doc);
                }
                block_upper_bound -=
                    ordered_cursors[i]->max_score() - ordered_cursors[i]->block_max_score();
                if (!m_topk.would_enter(score + block_upper_bound)) {
                    // brz_1_cnt
                    /*******/
                    // this_query_stats.br1_cnt_total++;
                    /*******/
                    break;
                }
            }
            if (m_topk.would_enter(score + block_upper_bound)) {
                // try to complete evaluation with non-essential lists
                // p4_cnt
                /*******/
                // this_query_stats.p4_cnt_total++;
                /*******/                

                for (size_t i = non_essential_lists - 1; i + 1 > 0; --i) {
                    // f3_cnt
                    /**********/
                    // this_query_stats.f3_cnt_total++;
                    /**********/

                    ordered_cursors[i]->next_geq(cur_doc);
                    if (ordered_cursors[i]->docid() == cur_doc) {
                        // p5_cnt
                        /*******/
                        // query_stat_logging[qid].p5_cnt_total++;
                        /*******/                        
                        auto s = ordered_cursors[i]->score();
                        block_upper_bound += s;
                    }
                    block_upper_bound -= ordered_cursors[i]->block_max_score();

                    if (!m_topk.would_enter(score + block_upper_bound)) {
                        // brz_2_cnt
                        /*******/
                        // query_stat_logging[qid].br2_cnt_total++;
                        /*******/                        
                        break;
                    }
                }
                score += block_upper_bound;
            }
            if (m_topk.insert(score, cur_doc)) {
                // update non-essential lists
                // p6_cnt
                /*******/
                // this_query_stats.p6_cnt_total++;
                /*******/   
                while (non_essential_lists < ordered_cursors.size()
                       && !m_topk.would_enter(upper_bounds[non_essential_lists])) {
                    // p7_cnt
                    /**********/
                    // this_query_stats.p7_cnt_total++;
                    /**********/
                    non_essential_lists += 1;
                }
            }
            cur_doc = next_doc;
        }

        /**********/
        auto end_alg_exec = std::chrono::steady_clock::now();

        double alg_prep_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(start_alg_exec - start_alg).count();

        double alg_while_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_alg_exec - start_alg_exec).count();

        
        double alg_total_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_alg_exec - start_alg).count();

        this_query_stats.alg_prep_ms = alg_prep_ms;
        this_query_stats.alg_while_ms = alg_while_ms;        
        this_query_stats.alg_total_ms = alg_total_ms;

        // spdlog::info("Time taken to for running the alg on this query: {}ms", alg_exec_ms);
        /**********/

        /**********/
        this_query_stats.non_ess_val = non_essential_lists;
        /**********/
    }

    std::vector<typename topk_queue::entry_type> const& topk() const { return m_topk.topk(); }

  private:
    topk_queue& m_topk;
};
}  // namespace pisa
