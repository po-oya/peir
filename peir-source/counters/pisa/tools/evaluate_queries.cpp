#include <iostream>
#include <string>
#include <optional>

#include <CLI/CLI.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <functional>
#include <mappable/mapper.hpp>
#include <mio/mmap.hpp>
#include <range/v3/view/enumerate.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <tbb/global_control.h>
#include <tbb/parallel_for.h>

#include "accumulator/lazy_accumulator.hpp"
#include "accumulator/simple_accumulator.hpp"
#include "app.hpp"
#include "cursor/block_max_scored_cursor.hpp"
#include "cursor/max_scored_cursor.hpp"
#include "cursor/scored_cursor.hpp"
#include "index_types.hpp"
#include "query/algorithm/block_max_maxscore_query.hpp"
#include "query/algorithm/block_max_ranked_and_query.hpp"
#include "query/algorithm/block_max_wand_query.hpp"
#include "query/algorithm/maxscore_query.hpp"
#include "query/algorithm/ranked_and_query.hpp"
#include "query/algorithm/ranked_or_query.hpp"
#include "query/algorithm/ranked_or_taat_query.hpp"
#include "query/algorithm/wand_query.hpp"
#include "scorer/scorer.hpp"
#include "wand_data.hpp"
#include "wand_data_compressed.hpp"
#include "wand_data_raw.hpp"
#include "util/measure_pars.hpp"


using namespace pisa;
using ranges::views::enumerate;

template <typename IndexType, typename WandType>
void evaluate_queries(
    const std::string& index_filename,
    const std::string& wand_data_filename,
    const std::vector<Query>& queries,
    const std::optional<std::string>& thresholds_filename,
    std::string const& type,
    std::string const& query_type,
    uint64_t k,
    std::string const& documents_filename,
    ScorerParams const& scorer_params,
    const bool weighted,
    std::string const& run_id,
    std::string const& iteration
) {
    IndexType index(MemorySource::mapped_file(index_filename));
    WandType const wdata(MemorySource::mapped_file(wand_data_filename));

    auto scorer = scorer::from_params(scorer_params, wdata);
    std::function<std::vector<typename topk_queue::entry_type>(Query)> query_fun;

    if (query_type == "wand") {
        query_fun = [&](Query query) {
            topk_queue topk(k);
            wand_query wand_q(topk);
            wand_q(make_max_scored_cursors(index, wdata, *scorer, query, weighted), index.num_docs());
            topk.finalize();
            return topk.topk();
        };
    } else if (query_type == "block_max_wand") {
        query_fun = [&](Query query) {
            topk_queue topk(k);
            block_max_wand_query block_max_wand_q(topk);
            block_max_wand_q(
                make_block_max_scored_cursors(index, wdata, *scorer, query, weighted),
                index.num_docs()
            );
            topk.finalize();
            return topk.topk();
        };
    } else if (query_type == "block_max_maxscore") {
        query_fun = [&](Query query) {
            topk_queue topk(k);
            auto tqid_string_view = query.id();
            std::string tqid(tqid_string_view.value_or(std::to_string(42))); // Setting 42 as an option for query ID, in case there is no value found.
            block_max_maxscore_query block_max_maxscore_q(topk, tqid);
            block_max_maxscore_q(
                make_block_max_scored_cursors(index, wdata, *scorer, query, weighted),
                index.num_docs()
            );
            topk.finalize();
            return topk.topk();
        };
    } else if (query_type == "block_max_ranked_and") {
        query_fun = [&](Query query) {
            topk_queue topk(k);
            block_max_ranked_and_query block_max_ranked_and_q(topk);
            block_max_ranked_and_q(
                make_block_max_scored_cursors(index, wdata, *scorer, query, weighted),
                index.num_docs()
            );
            topk.finalize();
            return topk.topk();
        };
    } else if (query_type == "ranked_and") {
        query_fun = [&](Query query) {
            topk_queue topk(k);
            ranked_and_query ranked_and_q(topk);
            ranked_and_q(make_scored_cursors(index, *scorer, query, weighted), index.num_docs());
            topk.finalize();
            return topk.topk();
        };
    } else if (query_type == "ranked_or") {
        query_fun = [&](Query query) {
            topk_queue topk(k);
            ranked_or_query ranked_or_q(topk);
            ranked_or_q(make_scored_cursors(index, *scorer, query, weighted), index.num_docs());
            topk.finalize();
            return topk.topk();
        };
    } else if (query_type == "maxscore") {
        query_fun = [&](Query query) {
            topk_queue topk(k);
            maxscore_query maxscore_q(topk);
            maxscore_q(
                make_max_scored_cursors(index, wdata, *scorer, query, weighted), index.num_docs()
            );
            topk.finalize();
            return topk.topk();
        };
    } else if (query_type == "ranked_or_taat") {
        query_fun = [&, accumulator = SimpleAccumulator(index.num_docs())](Query query) mutable {
            topk_queue topk(k);
            ranked_or_taat_query ranked_or_taat_q(topk);
            ranked_or_taat_q(
                make_scored_cursors(index, *scorer, query, weighted), index.num_docs(), accumulator
            );
            topk.finalize();
            return topk.topk();
        };
    } else if (query_type == "ranked_or_taat_lazy") {
        query_fun = [&, accumulator = LazyAccumulator<4>(index.num_docs())](Query query) mutable {
            topk_queue topk(k);
            ranked_or_taat_query ranked_or_taat_q(topk);
            ranked_or_taat_q(
                make_scored_cursors(index, *scorer, query, weighted), index.num_docs(), accumulator
            );
            topk.finalize();
            return topk.topk();
        };
    } else {
        spdlog::error("Unsupported query type: {}", query_type);
    }

    auto source = std::make_shared<mio::mmap_source>(documents_filename.c_str());
    auto docmap = Payload_Vector<>::from(*source);

    /* ********************* */
    size_t number_of_queries = queries.size();
    spdlog::info("Allocating space to query_stat_logging vector ...");
    
    for(size_t qitr = 0; qitr < number_of_queries; qitr++){
        
        Query const& tq = queries[qitr];
        auto tqid_string_view = tq.id();
        std::string tqid(tqid_string_view.value_or(std::to_string(qitr)));
        block_max_score_query_stat_logging query_log(tqid, tq.terms().size(), 0);
        query_stat_logging[tqid] = query_log;
    }
    /* ********************* */

    std::vector<std::vector<typename topk_queue::entry_type>> raw_results(queries.size());
    auto start_batch = std::chrono::steady_clock::now();
    tbb::parallel_for(size_t(0), queries.size(), [&, query_fun](size_t query_idx) {
        raw_results[query_idx] = query_fun(queries[query_idx]);
    });
    auto end_batch = std::chrono::steady_clock::now();

    for (size_t query_idx = 0; query_idx < raw_results.size(); ++query_idx) {
        auto results = raw_results[query_idx];
        auto qid = queries[query_idx].id();
        std::string tqid(qid.value_or(std::to_string(query_idx)));
        std::cout << fmt::format(
        "qid:{},\ttermcnt:{},\twcnt:{},\tnon_es:{},\toc_size{},\tf1:{},\tf2:{},\tf3:{},\tp1:{},\tp2:{},\tp3:{},\tp4:{},\tp5:{},\tp6:{},\tp7:{},\tbr1:{},\tbr2:{},\n",
            tqid,
            query_stat_logging[tqid].term_cnt,
            query_stat_logging[tqid].while_cnt,
            query_stat_logging[tqid].non_ess_val,
            query_stat_logging[tqid].oc_size,
            query_stat_logging[tqid].f1_cnt_total,
            query_stat_logging[tqid].f2_cnt_total,
            query_stat_logging[tqid].f3_cnt_total,
            query_stat_logging[tqid].p1_cnt_total,
            query_stat_logging[tqid].p2_cnt_total,
            query_stat_logging[tqid].p3_cnt_total,
            query_stat_logging[tqid].p4_cnt_total,
            query_stat_logging[tqid].p5_cnt_total,
            query_stat_logging[tqid].p6_cnt_total,
            query_stat_logging[tqid].p7_cnt_total,
            query_stat_logging[tqid].br1_cnt_total,
            query_stat_logging[tqid].br2_cnt_total
        );
    }
    auto end_print = std::chrono::steady_clock::now();
    double batch_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_batch - start_batch).count();
    double batch_with_print_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_print - start_batch).count();
    spdlog::info("Time taken to process queries: {}ms", batch_ms);
    spdlog::info("Time taken to process queries with printing: {}ms", batch_with_print_ms);
}

using wand_raw_index = wand_data<wand_data_raw>;
using wand_uniform_index = wand_data<wand_data_compressed<>>;
using wand_uniform_index_quantized = wand_data<wand_data_compressed<PayloadType::Quantized>>;

int main(int argc, const char** argv) {
    spdlog::set_default_logger(spdlog::stderr_color_mt("default"));

    std::string documents_file;
    std::string run_id = "R0";
    bool quantized = false;

    App<arg::Index,
        arg::WandData<arg::WandMode::Required>,
        arg::Query<arg::QueryMode::Ranked>,
        arg::Algorithm,
        arg::Scorer,
        arg::Thresholds,
        arg::Threads,
        arg::LogLevel>
        app{"Retrieves query results in TREC format."};
    app.add_option("-r,--run", run_id, "Run identifier");
    app.add_option("--documents", documents_file, "Document lexicon")->required();
    app.add_flag("--quantized", quantized, "Quantized scores");

    CLI11_PARSE(app, argc, argv);

    spdlog::set_level(app.log_level());
    tbb::global_control control(tbb::global_control::max_allowed_parallelism, app.threads() + 1);
    spdlog::info("Number of worker threads: {}", app.threads());

    if (run_id.empty()) {
        run_id = "PISA";
    }

    auto iteration = "Q0";


    auto params = std::make_tuple(
        app.index_filename(),
        app.wand_data_path(),
        app.queries(),
        app.thresholds_file(),
        app.index_encoding(),
        app.algorithm(),
        app.k(),
        documents_file,
        app.scorer_params(),
        app.weighted(),
        run_id,
        iteration
    );

    /**/
    if (false) {  // NOLINT
#define LOOP_BODY(R, DATA, T)                                                                       \
    }                                                                                               \
    else if (app.index_encoding() == BOOST_PP_STRINGIZE(T)) {                                       \
        if (app.is_wand_compressed()) {                                                             \
            if (quantized) {                                                                        \
                std::apply(                                                                         \
                    evaluate_queries<BOOST_PP_CAT(T, _index), wand_uniform_index_quantized>, params \
                );                                                                                  \
            } else {                                                                                \
                std::apply(evaluate_queries<BOOST_PP_CAT(T, _index), wand_uniform_index>, params);  \
            }                                                                                       \
        } else {                                                                                    \
            std::apply(evaluate_queries<BOOST_PP_CAT(T, _index), wand_raw_index>, params);          \
        }                                                                                           \
        /**/

        BOOST_PP_SEQ_FOR_EACH(LOOP_BODY, _, PISA_INDEX_TYPES);
#undef LOOP_BODY
    } else {
        spdlog::error("Unknown type {}", app.index_encoding());
    }
}
