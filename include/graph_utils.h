#pragma once

#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include <boost/algorithm/string.hpp>
#include <boost/graph/breadth_first_search.hpp>
#include <spdlog/fmt/fmt.h>

#include "command.h"
#include "graph.h"
#include "grid.h"
#include "logger.h"
#include "subrange.h"
#include "types.h"

namespace celerity {

namespace graph_utils {

	using task_vertices = std::pair<vertex, vertex>;

	template <typename Functor>
	bool call_for_vertex_fn(const Functor& fn, vertex v, std::true_type) {
		return fn(v);
	}

	template <typename Functor>
	bool call_for_vertex_fn(const Functor& fn, vertex v, std::false_type) {
		fn(v);
		return true;
	}

	/**
	 * Calls a functor on every predecessor of vertex v within the graph.
	 * The functor can optionally return a boolean indicating whether the
	 * loop should abort.
	 *
	 * Returns false if the loop was aborted.
	 */
	template <typename Graph, typename Functor>
	bool for_predecessors(const Graph& graph, vertex v, const Functor& f) {
		typename boost::graph_traits<Graph>::in_edge_iterator eit, eit_end;
		for(std::tie(eit, eit_end) = boost::in_edges(v, graph); eit != eit_end; ++eit) {
			vertex pre = boost::source(*eit, graph);
			if(call_for_vertex_fn(f, pre, std::is_same<bool, decltype(f(pre))>()) == false) { return false; }
		}
		return true;
	}

	/**
	 * Calls a functor on every successor of vertex v within the graph.
	 * The functor can optionally return a boolean indicating whether the
	 * loop should abort.
	 *
	 * Returns false if the loop was aborted.
	 */
	template <typename Graph, typename Functor>
	bool for_successors(const Graph& graph, vertex v, const Functor& f) {
		typename boost::graph_traits<Graph>::out_edge_iterator eit, eit_end;
		for(std::tie(eit, eit_end) = boost::out_edges(v, graph); eit != eit_end; ++eit) {
			vertex suc = boost::target(*eit, graph);
			if(call_for_vertex_fn(f, suc, std::is_same<bool, decltype(f(suc))>()) == false) { return false; }
		}
		return true;
	}

	// Note that we don't check whether the edge u->v actually existed
	template <typename Graph>
	vertex insert_vertex_on_edge(vertex u, vertex v, Graph& graph) {
		const auto e = boost::edge(u, v, graph);
		const auto w = boost::add_vertex(graph);
		boost::remove_edge(u, v, graph);
		boost::add_edge(u, w, graph);
		boost::add_edge(w, v, graph);
		return w;
	}

	class abort_search_exception : public std::runtime_error {
	  public:
		abort_search_exception() : std::runtime_error("Abort search (not an error)") {}
	};

	template <typename Functor>
	class bfs_visitor : public boost::default_bfs_visitor {
	  public:
		bfs_visitor(Functor f) : f(f) {}

		template <typename Graph>
		void discover_vertex(vertex v, const Graph& graph) const {
			if(f(v, graph) == true) { throw abort_search_exception(); }
		}

	  private:
		Functor f;
	};

	/**
	 * Search vertices using a breadth-first-search.
	 * The functor receives the current vertex as well as the graph by reference.
	 * The search is aborted if the functor returns true.
	 */
	template <typename Graph, typename Functor>
	void search_vertex_bf(vertex start, const Graph& graph, Functor f) {
		try {
			bfs_visitor<Functor> vis(f);
			boost::breadth_first_search(graph, start, boost::visitor(vis));
		} catch(abort_search_exception&) {
			// Nop
		}
	}

	task_vertices add_task(task_id tid, const task_dag& tdag, command_dag& cdag);

	template <int Dims>
	vertex add_compute_cmd(node_id nid, const task_vertices& tv, const subrange<Dims>& chunk, command_dag& cdag) {
		const auto v = boost::add_vertex(cdag);
		boost::add_edge(tv.first, v, cdag);
		boost::add_edge(v, tv.second, cdag);
		cdag[v].cmd = command::COMPUTE;
		cdag[v].nid = nid;
		cdag[v].tid = cdag[tv.first].tid;
		cdag[v].label = fmt::format("Node {}:\\nCOMPUTE {}", nid, detail::subrange_to_grid_region(chunk));
		cdag[v].data.compute.chunk = command_subrange(chunk);
		return v;
	}

	vertex add_master_access_cmd(const task_vertices& tv, command_dag& cdag);

	template <std::size_t Dims>
	vertex add_pull_cmd(node_id nid, node_id source_nid, buffer_id bid, const task_vertices& tv, const task_vertices& source_tv, vertex req_cmd,
	    const GridBox<Dims>& req, command_dag& cdag) {
		assert(cdag[req_cmd].cmd == command::COMPUTE || cdag[req_cmd].cmd == command::MASTER_ACCESS);
		const auto v = graph_utils::insert_vertex_on_edge(tv.first, req_cmd, cdag);
		cdag[v].cmd = command::PULL;
		cdag[v].nid = nid;
		cdag[v].tid = cdag[tv.first].tid;
		cdag[v].label = fmt::format("Node {}:\\nPULL {} from {}\\n {}", nid, bid, source_nid, req);
		cdag[v].data.pull.bid = bid;
		cdag[v].data.pull.source = source_nid;
		cdag[v].data.pull.subrange = command_subrange(detail::grid_box_to_subrange(req));

		// Find the compute / master access command for the source node in the writing task (or this
		// task, if no writing task has been found)
		vertex source_command_v = std::numeric_limits<size_t>::max();
		search_vertex_bf(source_tv.first, cdag, [source_nid, source_tv, &source_command_v](vertex v, const command_dag& cdag) {
			// FIXME: We have some special casing here for master access:
			// Master access only executes on the master node, which is (generally) not the source node. If the master access
			// is not in a sibling set with some writing task, we won't be able to find a compute comand for source_nid.
			// A proper solution to this will also handle the fact that in the futue we won't necessarily split every task
			// over all nodes.
			if(cdag[v].cmd == command::MASTER_ACCESS || cdag[v].cmd == command::COMPUTE && cdag[v].nid == source_nid) {
				source_command_v = v;
				return true;
			}
			return false;
		});

		// If the buffer is on the master node, chances are there isn't any master access command in the (source) task.
		// In this case, we simply add the await pull anywhere in the (source) task.
		if(source_command_v == std::numeric_limits<size_t>::max() && source_nid == 0) { source_command_v = source_tv.second; }
		assert(source_command_v != std::numeric_limits<size_t>::max());

		const auto w = graph_utils::insert_vertex_on_edge(source_tv.first, source_command_v, cdag);
		cdag[w].cmd = command::AWAIT_PULL;
		cdag[w].nid = source_nid;
		cdag[w].tid = cdag[source_tv.first].tid;
		cdag[w].label = fmt::format("Node {}:\\nAWAIT PULL {} by {}\\n {}", source_nid, bid, nid, req);
		cdag[w].data.await_pull.bid = bid;
		cdag[w].data.await_pull.target = nid;
		cdag[w].data.await_pull.target_tid = cdag[tv.first].tid;
		cdag[w].data.await_pull.subrange = command_subrange(detail::grid_box_to_subrange(req));

		// Add edges in both directions
		boost::add_edge(w, v, cdag);
		boost::add_edge(v, w, cdag);

		return v;
	}

	/**
	 * Returns a set of tasks that
	 *  (1) have all their requirements satisfied (i.e., all predecessors are
	 *      marked as processed)
	 *  (2) don't have any unsatisfied siblings.
	 *
	 *  Note that "siblingness" can be transitive, meaning that not every pair
	 *  of returned tasks necessarily has common parents. All siblings are
	 *  however connected through some child->parent->child->[...] chain.
	 */
	std::vector<task_id> get_satisfied_sibling_set(const task_dag& tdag);

	void mark_as_processed(task_id tid, task_dag& tdag);


	// --------------------------- Graph printing ---------------------------


	template <typename Graph, typename VertexPropertiesWriter, typename EdgePropertiesWriter>
	void write_graph_mux(const Graph& g, VertexPropertiesWriter vpw, EdgePropertiesWriter epw, std::shared_ptr<logger> graph_logger) {
		std::stringstream ss;
		write_graphviz(ss, g, vpw, epw);
		auto str = ss.str();
		boost::replace_all(str, "\n", "\\n");
		boost::replace_all(str, "\"", "\\\"");
		graph_logger->info(logger_map({{"name", g[boost::graph_bundle].name}, {"data", str}}));
	}

	void print_graph(const celerity::task_dag& tdag, std::shared_ptr<logger> graph_logger);
	void print_graph(const celerity::command_dag& cdag, std::shared_ptr<logger> graph_logger);

} // namespace graph_utils

} // namespace celerity
