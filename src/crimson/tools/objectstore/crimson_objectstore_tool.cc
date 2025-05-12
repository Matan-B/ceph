// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/program_options/parsers.hpp>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <seastar/core/app-template.hh>
#include <seastar/core/signal.hh>
#include <string>
#include <string_view>
#include <vector>
#include <sys/stat.h>
#include <sstream>

#include "common/Formatter.h"
#include "common/hobject.h"
#include "crimson/common/errorator.h"
#include "crimson/osd/stop_signal.h"
#include "crimson/osd/osd_meta.h"

#include "objectstore_tool.h"
#include "osd/osd_types.h"
#include "crimson/common/config_proxy.h"
#include "crimson/os/futurized_collection.h"
#include "crimson/os/futurized_store.h"
#include "seastar/util/closeable.hh"
#include "seastar/util/log.hh"
#include "crimson/os/seastore/segment_manager.h"
#include "include/expected.hpp"

namespace bpo = boost::program_options;
using crimson::common::sharded_conf;
using crimson::common::local_conf;

using namespace crimson::tools::kvstore;

enum class operation_type_t {
  LIST_PGS,
  LIST_OBJECTS, 
  LIST_OMAP,
  GET_OMAP,
  SET_OMAP,
  REMOVE_OMAP,
};

std::string to_string(operation_type_t op) {
  switch (op) {
    case operation_type_t::LIST_PGS: return "list-pgs";
    case operation_type_t::LIST_OBJECTS: return "list-objects";
    case operation_type_t::LIST_OMAP: return "list-omap";
    case operation_type_t::GET_OMAP: return "get-omap";
    case operation_type_t::SET_OMAP: return "set-omap";
    case operation_type_t::REMOVE_OMAP: return "remove-omap";
    default: return "unknown";
  }
}

tl::expected<operation_type_t, std::string> parse_operation(const std::string& op_str) {
  if (op_str == "list-pgs") return operation_type_t::LIST_PGS;
  if (op_str == "list-objects") return operation_type_t::LIST_OBJECTS;
  if (op_str == "list-omap") return operation_type_t::LIST_OMAP;
  if (op_str == "get-omap") return operation_type_t::GET_OMAP;
  if (op_str == "set-omap") return operation_type_t::SET_OMAP;
  if (op_str == "remove-omap") return operation_type_t::REMOVE_OMAP;
  return tl::unexpected("Unknown operation: " + op_str);
}

struct operation_params_t {
  operation_type_t op;
  std::optional<pg_t> pgid;
  std::optional<ghobject_t> object;
  std::optional<std::string> omap_key;
  std::optional<std::string> omap_start;
  std::optional<std::string> input_file;
  std::optional<std::string> output_file;
};

struct objectstore_config_t {
  std::string data_path;
  std::string store_type;
  std::string format;
  std::optional<operation_params_t> operation;
  coll_t coll;
  spg_t pgid;
  ghobject_t ghobj;
  bool debug = false;

  void populate_options(bpo::options_description &desc) {
    desc.add_options()
      ("data-path",
       bpo::value<std::string>(&data_path)->required(),
       "osd data path (required)")
      ("store-type",
       bpo::value<std::string>(&store_type)->required(),
       "store type, e.g seastore (required)")
      ("debug",
       bpo::bool_switch(&debug),
       "set logger level as debug")
      ("op",
       bpo::value<std::string>(),
       "operation to perform: list-pgs, list-objects, list-omap, get-omap, set-omap, remove-omap")
      ("format",
       bpo::value<std::string>(&format)->default_value("json-pretty"),
       "Output format which may be json, json-pretty, xml, xml-pretty");
  }
};

tl::expected<pg_t, std::string> parse_pgid(const std::string& pgid_str) {
  pg_t pgid;
  if (!pgid.parse(pgid_str.c_str())) {
    return tl::unexpected("Invalid pgid: " + pgid_str);
  }
  return pgid;
}

tl::expected<ghobject_t, std::string> parse_object(const std::string& obj_str) {
  ghobject_t obj;
  if (!obj.parse(obj_str)) {
    return tl::unexpected("Invalid object: " + obj_str);
  }
  return obj;
}

tl::expected<std::pair<pg_t, ghobject_t>, std::string> parse_pg_and_object(const std::vector<std::string>& args, size_t start_idx) {
  if (args.size() < start_idx + 2) {
    return tl::unexpected("Insufficient arguments for pg and object");
  }

  auto pgid_result = parse_pgid(args[start_idx]);
  if (!pgid_result) {
    return tl::unexpected(pgid_result.error());
  }

  auto obj_result = parse_object(args[start_idx + 1]);
  if (!obj_result) {
    return tl::unexpected(obj_result.error());
  }

  return std::make_pair(*pgid_result, *obj_result);
}

tl::expected<operation_params_t, std::string> parse_operation_params(operation_type_t op, 
                                                                  const std::vector<std::string>& positional_args) {
  operation_params_t params;
  params.op = op;

  switch (op) {
    case operation_type_t::LIST_PGS:
      // No additional parameters needed
      break;

    case operation_type_t::LIST_OBJECTS:
      if (positional_args.size() < 1) {
        return tl::unexpected("list-objects requires <pgid>");
      }
      {
        auto pgid_result = parse_pgid(positional_args[0]);
        if (!pgid_result) {
          return tl::unexpected(pgid_result.error());
        }
        params.pgid = *pgid_result;
      }
      break;

    case operation_type_t::LIST_OMAP:
      if (positional_args.size() < 2) {
        return tl::unexpected("list-omap requires <pgid> <object>");
      }
      {
        auto pg_obj_result = parse_pg_and_object(positional_args, 0);
        if (!pg_obj_result) {
          return tl::unexpected(pg_obj_result.error());
        }
        auto [pgid, obj] = *pg_obj_result;
        params.pgid = pgid;
        params.object = obj;
      }
      if (positional_args.size() >= 3) {
        params.omap_start = positional_args[2];
      }
      break;

    case operation_type_t::GET_OMAP:
      if (positional_args.size() < 3) {
        return tl::unexpected("get-omap requires <pgid> <object> <key>");
      }
      {
        auto pg_obj_result = parse_pg_and_object(positional_args, 0);
        if (!pg_obj_result) {
          return tl::unexpected(pg_obj_result.error());
        }
        auto [pgid, obj] = *pg_obj_result;
        params.pgid = pgid;
        params.object = obj;
      }
      params.omap_key = positional_args[2];

      // Parse optional output file
      for (size_t i = 3; i < positional_args.size(); i++) {
        if (positional_args[i] == "out" && i + 1 < positional_args.size()) {
          params.output_file = positional_args[i + 1];
          i++; // Skip the filename in next iteration
        }
      }
      break;

    case operation_type_t::SET_OMAP:
      if (positional_args.size() < 3) {
        return tl::unexpected("set-omap requires <pgid> <object> <key>");
      }
      {
        auto pg_obj_result = parse_pg_and_object(positional_args, 0);
        if (!pg_obj_result) {
          return tl::unexpected(pg_obj_result.error());
        }
        auto [pgid, obj] = *pg_obj_result;
        params.pgid = pgid;
        params.object = obj;
      }
      params.omap_key = positional_args[2];

      // Parse optional input file
      for (size_t i = 3; i < positional_args.size(); i++) {
        if (positional_args[i] == "in" && i + 1 < positional_args.size()) {
          params.input_file = positional_args[i + 1];
          i++; // Skip the filename in next iteration
        }
      }
      break;

    case operation_type_t::REMOVE_OMAP:
      if (positional_args.size() < 3) {
        return tl::unexpected("remove-omap requires <pgid> <object> <key>");
      }
      {
        auto pg_obj_result = parse_pg_and_object(positional_args, 0);
        if (!pg_obj_result) {
          return tl::unexpected(pg_obj_result.error());
        }
        auto [pgid, obj] = *pg_obj_result;
        params.pgid = pgid;
        params.object = obj;
      }
      params.omap_key = positional_args[2];
      break;
  }

  return params;
}

void print_usage() {
  std::cout << "Usage: crimson_objectstore_tool --data-path <path> --store-type <type> --op <operation> [args...]\n\n";
  std::cout << "Required options:\n";
  std::cout << "  --data-path <path>    OSD data path\n";
  std::cout << "  --store-type <type>   Store type (e.g., seastore)\n";
  std::cout << "  --op <operation>      Operation to perform\n\n";
  std::cout << "Operations:\n";
  std::cout << "  list-pgs                                       List all PGs\n";
  std::cout << "  list-objects <pgid>                            List objects in PG\n";
  std::cout << "  list-omap <pgid> <object> [omap-start]         List omap keys\n";
  std::cout << "  get-omap <pgid> <object> <key> [out <file>]    Get omap value\n";
  std::cout << "  set-omap <pgid> <object> <key> in <file>       Set omap key-value from file\n";
  std::cout << "  remove-omap <pgid> <object> <key>              Remove omap key\n\n";
  std::cout << "Examples:\n";
  std::cout << "  crimson_objectstore_tool --data-path /var/lib/ceph/osd/ceph-0 --store-type seastore --op list-pgs\n";
  std::cout << "  crimson_objectstore_tool --data-path /var/lib/ceph/osd/ceph-0 --store-type seastore --op list-objects 1.0\n";
  std::cout << "  crimson_objectstore_tool --data-path /var/lib/ceph/osd/ceph-0 --store-type seastore --op set-omap 1.0 obj1 key1 in value.txt\n";
  std::cout << "  crimson_objectstore_tool --data-path /var/lib/ceph/osd/ceph-0 --store-type seastore --op get-omap 1.0 obj1 key1 out output.txt\n";
}

class SeastoreMetaReader {
private:
  std::string m_data_path;

  size_t get_filesystem_block_size(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
      return st.st_blksize;
    }
    return 4096;
  }

public:
  explicit SeastoreMetaReader(const std::string& path) : m_data_path(path) {}

  tl::expected<crimson::os::seastore::block_sm_superblock_t, std::string> load_seastore_superblock() {
    try {
      std::string block_path = m_data_path + "/block";

      size_t block_size = get_filesystem_block_size(block_path);

      std::ifstream file(block_path, std::ios::binary);
      if (!file.is_open()) {
        return tl::unexpected("Could not open block file: " + block_path);
      }

      std::vector<char> buf(block_size);
      file.read(buf.data(), block_size);

      if (!file.good() && !file.eof()) {
        return tl::unexpected("Could not read superblock from " + block_path);
      }

      bufferlist bl;
      bl.append(buf.data(), block_size);

      crimson::os::seastore::block_sm_superblock_t superblock;
      auto bliter = bl.cbegin();
      decode(superblock, bliter);

      ceph_assert(ceph::encoded_sizeof<crimson::os::seastore::block_sm_superblock_t>(superblock) <
                  block_size);

      return superblock;

    } catch (const std::exception& e) {
      return tl::unexpected("Could not read seastore superblock: " + std::string(e.what()));
    } catch (...) {
      return tl::unexpected("Could not read seastore superblock: unknown error");
    }
  }

  tl::expected<unsigned int, std::string> get_shard_count() {
    auto superblock_result = load_seastore_superblock();
    if (!superblock_result) {
      return tl::unexpected(superblock_result.error());
    }

    logger.debug("Read shard count from storage: {}", superblock_result->shard_num);
    return superblock_result->shard_num;
  }
};

static tl::expected<unsigned int, std::string>
read_shard_count_from_storage(const std::string& data_path,
                              const std::string& store_type)
{
  if (store_type != "seastore") {
    return tl::unexpected("Store type not supported for shard count reading");
  }

  SeastoreMetaReader meta_reader(data_path);
  return meta_reader.get_shard_count();
}

static tl::expected<std::vector<std::string>, std::string>
get_seastar_args_from_storage(const objectstore_config_t& config)
{
  auto shard_count_result = read_shard_count_from_storage(config.data_path,
                                                         config.store_type);

  if (!shard_count_result) {
    return tl::unexpected(shard_count_result.error());
  }

  // seastore case with valid shard count
  std::vector<std::string> seastar_args;
  seastar_args.emplace_back("--smp");
  seastar_args.emplace_back(std::to_string(*shard_count_result));
  seastar_args.emplace_back("--thread-affinity");
  seastar_args.emplace_back("0");

  logger.debug("Using shard configuration from storage: --smp {}", *shard_count_result);
  return seastar_args;
}

seastar::future<int> run_tool(StoreTool& st, objectstore_config_t& config) {
  std::unique_ptr<Formatter> formatter(
    Formatter::create(config.format));

  if (!config.operation.has_value()) {
    logger.error("No operation specified");
    co_return EXIT_FAILURE;
  }

  const auto& op = config.operation.value();

  // Setup collection and object for operations that need them
  if (op.op != operation_type_t::LIST_PGS) {
    if (op.pgid.has_value()) {
      config.pgid = spg_t(op.pgid.value());
      config.coll = coll_t(config.pgid);
    } else {
      logger.error("PG ID required for operation {}", to_string(op.op));
      co_return EXIT_FAILURE;
    }

    auto pgs = co_await st.list_pgs();
    auto it = std::find_if(pgs.begin(), pgs.end(),
      [&config](const auto& pg) { return pg.first == config.coll; });
    if (it == pgs.end()) {
      logger.error("PG '{}' not found", config.coll);
      co_return EXIT_FAILURE;
    }
    st.set_shard_id(it->second);

    if (op.op == operation_type_t::LIST_OMAP ||
        op.op == operation_type_t::GET_OMAP ||
        op.op == operation_type_t::SET_OMAP ||
        op.op == operation_type_t::REMOVE_OMAP) {
      if (op.object.has_value()) {
        config.ghobj = op.object.value();
      } else {
        logger.info("object name is empty, use pgmeta oid");
        config.ghobj = config.pgid.make_pgmeta_oid();
      }
    }
  }

  switch (op.op) {
    case operation_type_t::LIST_PGS: {
      auto pgs = co_await st.list_pgs();
      for (auto pg : pgs) {
        fmt::print(std::cout, "pg: {}, shard id: {}\n", pg.first, pg.second);
      }
      break;
    }

    case operation_type_t::LIST_OBJECTS: {
      ghobject_t next;
      do {
        auto objs = co_await st.list_objects(config.coll, next);
        next = std::get<1>(objs);
        for (auto obj : std::get<0>(objs)) {
          formatter->open_object_section("objects");
          formatter->dump_string("name", fmt::format("{}", obj));
          formatter->close_section();
        }
      } while (next != ghobject_t::get_max());
      formatter->flush(std::cout);
      break;
    }

    case operation_type_t::LIST_OMAP: {
      try {
        FuturizedStore::Shard::omap_values_t omaps =
          co_await st.omap_get_values(config.coll, config.ghobj, op.omap_start);
        if (omaps.empty()) {
          if (config.format == "json" ||
              config.format == "json-pretty") {
            formatter->open_array_section("omap_keys");
            formatter->close_section();
            formatter->flush(std::cout);
          } else {
            logger.info("No omap keys found");
          }
        } else {
          if (config.format == "json" ||
              config.format == "json-pretty") {
            formatter->open_array_section("omap_keys");
            for (const auto& omap : omaps) {
              formatter->dump_string("key", omap.first);
            }
            formatter->close_section();
            formatter->flush(std::cout);
          } else {
            for (const auto& omap : omaps) {
              fmt::print(std::cout, "{}", omap.first);
            }
          }
        }
      } catch (const std::exception& e) {
        logger.error("Error reading omap values: {}", e.what());
        logger.error("This may indicate storage corruption or version mismatch");
        co_return EXIT_FAILURE;
      }
      break;
    }

    case operation_type_t::SET_OMAP: {
      if (!op.omap_key.has_value()) {
        logger.error("omap-key is required for set-omap");
        co_return EXIT_FAILURE;
      }

      if (!op.input_file.has_value()) {
        logger.error("input-file is required for set-omap (use 'in <file>')");
        co_return EXIT_FAILURE;
      }

      std::string omap_value_input;
      std::ifstream infile(op.input_file.value(), std::ios::binary);
      if (!infile.is_open()) {
        logger.error("failed to open input-file '{}'", op.input_file.value());
        co_return EXIT_FAILURE;
      }
      std::stringstream buffer;
      buffer << infile.rdbuf();
      omap_value_input = buffer.str();

      bool success = co_await st.set_omap(
        config.coll, config.ghobj,
        op.omap_key.value(), omap_value_input);
      if (success) {
        fmt::print(std::cout, "set omap success: key={}, value from file={} ({} bytes)\n",
          op.omap_key.value(), op.input_file.value(), omap_value_input.size());
      } else {
        logger.error("set omap failed");
        co_return EXIT_FAILURE;
      }
      break;
    }

    case operation_type_t::GET_OMAP: {
      if (!op.omap_key.has_value()) {
        logger.error("omap-key is required for get-omap");
        co_return EXIT_FAILURE;
      }
      try {
        std::string omap_value = co_await st.get_omap(
          config.coll, config.ghobj, op.omap_key.value());
        if (!omap_value.empty()) {
          if (op.output_file.has_value()) {
            std::ofstream outfile(op.output_file.value(), std::ios::binary);
            if (!outfile.is_open()) {
              logger.error("failed to open output-file '{}' for writing", op.output_file.value());
              co_return EXIT_FAILURE;
            }
            outfile.write(omap_value.data(), omap_value.size());
            outfile.close();
            fmt::print(std::cout, "get omap success: key={}, value saved to {} ({} bytes)\n",
              op.omap_key.value(), op.output_file.value(), omap_value.size());
          } else {
            fmt::print(std::cout, "{}", omap_value);
          }
        } else {
          fmt::print(std::cout, "get omap failed\n");
          co_return EXIT_FAILURE;
        }
      } catch (const std::exception& e) {
        logger.error("Error reading omap value: {}", e.what());
        logger.error("This may indicate storage corruption or version mismatch");
        co_return EXIT_FAILURE;
      }
      break;
    }

    case operation_type_t::REMOVE_OMAP: {
      if (!op.omap_key.has_value()) {
        logger.error("omap-key is required for remove-omap");
        co_return EXIT_FAILURE;
      }
      try {
        bool success = co_await st.remove_omap(
          config.coll, config.ghobj, op.omap_key.value());
        if (success) {
          fmt::print(std::cout, "remove omap success: key={}\n",
            op.omap_key.value());
        } else {
          logger.error("remove omap failed");
          co_return EXIT_FAILURE;
        }
      } catch (const std::exception& e) {
        logger.error("Error removing omap value: {}", e.what());
        logger.error("This may indicate storage corruption or version mismatch");
        co_return EXIT_FAILURE;
      }
      break;
    }
  }

  std::cout.flush();
  co_return EXIT_SUCCESS;
}

int main(int argc, const char* argv[])
{
  bpo::options_description desc{"ObjectStore Tool Options"};

  objectstore_config_t config;
  config.populate_options(desc);

  desc.add_options()
    ("help,h", "produce help message");

  bpo::variables_map vm;
  std::vector<std::string> unrecognized_options;
  try {
    auto parsed = bpo::command_line_parser(argc, argv)
      .options(desc)
      .allow_unregistered()
      .run();
    bpo::store(parsed, vm);

    if (vm.count("help")) {
      print_usage();
      return EXIT_SUCCESS;
    }

    bpo::notify(vm);
    unrecognized_options =
      bpo::collect_unrecognized(parsed.options, bpo::include_positional);
  } catch (const bpo::error& e) {
    logger.error("Error: {}", e.what());
    print_usage();
    return EXIT_FAILURE;
  }

  // Parse operation and positional arguments
  if (!vm.count("op")) {
    logger.error("Operation (--op) is required");
    print_usage();
    return EXIT_FAILURE;
  }

  auto op_type_result = parse_operation(vm["op"].as<std::string>());
  if (!op_type_result) {
    logger.error("Invalid operation: {}", op_type_result.error());
    print_usage();
    return EXIT_FAILURE;
  }

  auto op_params_result = parse_operation_params(*op_type_result, unrecognized_options);
  if (!op_params_result) {
    logger.error("Invalid arguments: {}", op_params_result.error());
    print_usage();
    return EXIT_FAILURE;
  }
  config.operation = *op_params_result;

  auto seastar_args_result = get_seastar_args_from_storage(config);
  if (!seastar_args_result) {
    logger.error("Failed to get seastar arguments: {}", seastar_args_result.error());
    return EXIT_FAILURE;
  }
  auto& seastar_args = *seastar_args_result;

  seastar::app_template::config app_cfg;
  app_cfg.name = "crimson-objectstore-tool";
  app_cfg.auto_handle_sigint_sigterm = true;
  seastar::app_template app(std::move(app_cfg));

  std::vector<char*> seastar_argv;
  seastar_argv.push_back(const_cast<char*>(argv[0]));
  for (auto& arg : seastar_args) {
    seastar_argv.push_back(const_cast<char*>(arg.c_str()));
  }

  try {
    return app.run(
      seastar_argv.size(),
      seastar_argv.data(),
      [&] {
        return seastar::async([&] {
          try {
          sharded_conf().start(EntityName{}, std::string_view{"ceph"}).get();
          auto stop_conf = seastar::deferred_stop(sharded_conf());
          local_conf().start().get();
          seastar_apps_lib::stop_signal should_stop;
          if (config.debug) {
            seastar::global_logger_registry().set_all_loggers_level(
              seastar::log_level::debug
            );
          }
          logger.set_ostream_enabled(true);

          auto store = crimson::os::FuturizedStore::create(
            config.store_type,
            config.data_path,
            local_conf().get_config_values());
          store->start().get();
          store->mount().handle_error(
            crimson::stateful_ec::assert_failure(fmt::format(
              "error mounting object store in {}",
              config.data_path
            ).c_str())
          ).get();
          StoreTool st(std::move(store));
          auto stop_st = seastar::deferred_stop(st);
          int ret = run_tool(st, config).get();
          return ret;
        } catch (...) {
          logger.error("startup failed: {}", std::current_exception());
          return EXIT_FAILURE;
        }
        });
      }
    );
  } catch (...) {
    logger.error("FATAL: Exception during startup, aborting: {}",
               std::current_exception());
    return EXIT_FAILURE;
  }
}
