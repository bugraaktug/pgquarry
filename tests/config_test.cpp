#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "config.hpp"

using namespace pgquarry;

namespace {

// Set by CMake to the absolute path of tests/fixtures — avoids depending on
// the test binary's working directory.
std::string fixture(const std::string& name)
{
    return std::string(TESTS_FIXTURES_DIR) + "/" + name;
}

bool has_error(const std::vector<std::string>& errors, const std::string& needle)
{
    return std::any_of(errors.begin(), errors.end(), [&](const std::string& e) {
        return e.find(needle) != std::string::npos;
    });
}

// Baseline AppConfig that passes validate() outright — individual tests
// mutate one field at a time off of this.
AppConfig valid_config()
{
    AppConfig cfg;
    cfg.conninfo = "host=localhost dbname=test";
    cfg.embedding.model_path = fixture("readable_model.gguf");
    return cfg;
}

} // namespace

TEST_CASE("validate: a config with every field left at its defaults, minus the two required fields, passes")
{
    AppConfig cfg = valid_config();
    CHECK(cfg.validate().empty());
}

TEST_CASE("validate: [worker] conninfo is required")
{
    AppConfig cfg = valid_config();
    cfg.conninfo.clear();
    CHECK(has_error(cfg.validate(), "[worker] conninfo is required"));
}

TEST_CASE("validate: [worker] model_path")
{
    SUBCASE("empty is rejected")
    {
        AppConfig cfg = valid_config();
        cfg.embedding.model_path.clear();
        CHECK(has_error(cfg.validate(), "[worker] model_path is required"));
    }

    SUBCASE("nonexistent path is rejected")
    {
        AppConfig cfg = valid_config();
        cfg.embedding.model_path = fixture("does_not_exist.gguf");
        CHECK(has_error(cfg.validate(), "model_path does not exist"));
    }

    SUBCASE("a directory is rejected as not a regular file")
    {
        AppConfig cfg = valid_config();
        cfg.embedding.model_path = TESTS_FIXTURES_DIR;
        CHECK(has_error(cfg.validate(), "not a regular file"));
    }

    SUBCASE("a 0-byte file is rejected")
    {
        AppConfig cfg = valid_config();
        cfg.embedding.model_path = fixture("empty_model.gguf");
        CHECK(has_error(cfg.validate(), "empty (0-byte) file"));
    }

    SUBCASE("an unreadable file is rejected")
    {
        if (geteuid() == 0) {
            // root bypasses file permission bits — nothing to assert here.
            return;
        }

        namespace fs = std::filesystem;
        fs::path path = fs::temp_directory_path() / "pgquarry_test_unreadable.gguf";
        {
            std::ofstream f(path);
            f << "some bytes";
        }
        chmod(path.c_str(), 0000);

        AppConfig cfg = valid_config();
        cfg.embedding.model_path = path.string();
        CHECK(has_error(cfg.validate(), "not readable by the current user"));

        chmod(path.c_str(), 0644);
        fs::remove(path);
    }
}

TEST_CASE("validate: embedding dimensions/threads/ctx/batch/gpu-layers")
{
    SUBCASE("dimensions must be > 0")
    {
        AppConfig cfg = valid_config();
        cfg.embedding.dimensions = 0;
        CHECK(has_error(cfg.validate(), "[worker] dimensions must be > 0"));
    }
    SUBCASE("n_threads must be > 0")
    {
        AppConfig cfg = valid_config();
        cfg.embedding.n_threads = 0;
        CHECK(has_error(cfg.validate(), "[worker] n_threads must be > 0"));
    }
    SUBCASE("n_ctx must be > 0")
    {
        AppConfig cfg = valid_config();
        cfg.embedding.n_ctx = -1;
        CHECK(has_error(cfg.validate(), "[worker] n_ctx must be > 0"));
    }
    SUBCASE("batch_size must be > 0")
    {
        AppConfig cfg = valid_config();
        cfg.embedding.max_batch_size = 0;
        CHECK(has_error(cfg.validate(), "[worker] batch_size must be > 0"));
    }
    SUBCASE("n_gpu_layers must be >= 0")
    {
        AppConfig cfg = valid_config();
        cfg.embedding.n_gpu_layers = -1;
        CHECK(has_error(cfg.validate(), "[worker] n_gpu_layers must be >= 0"));
    }
}

TEST_CASE("validate: [worker] poll_interval_ms must be > 0")
{
    AppConfig cfg = valid_config();
    cfg.poll_interval_ms = 0;
    CHECK(has_error(cfg.validate(), "[worker] poll_interval_ms must be > 0"));
}

TEST_CASE("validate: [retention] purge_after")
{
    SUBCASE("empty and '0' are both valid (purging disabled)")
    {
        AppConfig cfg = valid_config();
        cfg.retention.purge_after = "";
        CHECK(cfg.validate().empty());
        cfg.retention.purge_after = "0";
        CHECK(cfg.validate().empty());
    }

    SUBCASE("a well-formed duration is valid")
    {
        for (const std::string v : {"7d", "12h", "30m", "45s"}) {
            AppConfig cfg = valid_config();
            cfg.retention.purge_after = v;
            CAPTURE(v);
            CHECK(cfg.validate().empty());
        }
    }

    SUBCASE("a malformed duration is rejected")
    {
        for (const std::string v : {"7", "7x", "d7", "-3d", "banana"}) {
            AppConfig cfg = valid_config();
            cfg.retention.purge_after = v;
            CAPTURE(v);
            CHECK(has_error(cfg.validate(), "[retention] purge_after must be"));
        }
    }
}

TEST_CASE("validate: [logging] log_level must be one of the known levels")
{
    AppConfig cfg = valid_config();
    cfg.logging.log_level = "verbose";
    CHECK(has_error(cfg.validate(), "[logging] log_level must be one of"));
}

TEST_CASE("validate: [logging] max_size_mb and max_files must be > 0")
{
    AppConfig cfg = valid_config();
    cfg.logging.max_size_mb = 0;
    CHECK(has_error(cfg.validate(), "[logging] max_size_mb must be > 0"));

    cfg = valid_config();
    cfg.logging.max_files = 0;
    CHECK(has_error(cfg.validate(), "[logging] max_files must be > 0"));
}

TEST_CASE("parse_purge_after")
{
    SUBCASE("empty and '0' both mean 'disabled', parsed as 0 seconds")
    {
        CHECK(parse_purge_after("") == "0 seconds");
        CHECK(parse_purge_after("0") == "0 seconds");
    }

    SUBCASE("each unit maps to the matching Postgres interval word")
    {
        CHECK(parse_purge_after("7d") == "7 days");
        CHECK(parse_purge_after("12h") == "12 hours");
        CHECK(parse_purge_after("30m") == "30 minutes");
        CHECK(parse_purge_after("45s") == "45 seconds");
    }

    SUBCASE("malformed input returns nullopt")
    {
        for (const std::string v : {"7", "7x", "d7", "-3d", "banana", "d"}) {
            CAPTURE(v);
            CHECK(!parse_purge_after(v).has_value());
        }
    }
}
