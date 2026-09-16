// conf/crossy.cfg store: round trip, first start, broken lines, unknown keys kept; both save strategies
// (Rename: PC/R36S, Direct: SF2000 without rename, with a backup copy and a sync hook).
//   test_config.exe <scratch dir>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/config.h"

using namespace cr;

static int checks = 0, failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        std::printf("FAIL %s\n", what);
    }
}

static bool exists(const std::string &path)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}

static void writeText(const std::string &path, const char *text)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fputs(text, f);
    std::fclose(f);
}

static std::vector<std::string> synced;
static void recordSync(const char *path) { synced.push_back(path); }

static void commonChecks(const std::string &path, const char *mode)
{
    std::printf("mode %s\n", mode);
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());

    Config first;
    check(!first.load(path), "missing file reports false");
    check(first.getInt("highscore", 0) == 0, "missing key gives fallback");

    first.setInt("highscore", 42);
    first.set("character", " chicken ");
    first.set("future_key", "kept");
    check(first.save(path), "save creates the directory and file");

    Config second;
    check(second.load(path), "load after save");
    check(second.getInt("highscore", 0) == 42, "int round trip");
    check(second.get("character", "") == "chicken", "string trimmed");
    check(second.get("future_key", "") == "kept", "unknown key kept");

    second.setInt("highscore", 43);
    check(second.save(path), "save over an existing file");
    Config third;
    third.load(path);
    check(third.getInt("highscore", 0) == 43, "overwrite");
    check(third.get("future_key", "") == "kept", "unknown key survives a second save");

    std::remove((path + ".tmp").c_str());
    writeText(path, "# comment\r\ngarbage line\r\nhighscore = 12x\r\nvolume=7\r\n=nokey\r\n");
    Config broken;
    check(broken.load(path), "load CRLF file");
    check(broken.getInt("highscore", -1) == -1, "non-numeric int gives fallback");
    check(broken.getInt("volume", 0) == 7, "CRLF value parsed");
    check(broken.get("", "none") == "none", "empty key ignored");

    writeText(path, "# only a comment\n");
    Config comments;
    check(comments.load(path), "a file with only comments still loads");
    check(comments.getInt("volume", -1) == -1, "and has no settings");
}

int main(int argc, char **argv)
{
    std::string dir = argc > 1 ? argv[1] : ".";

    Config::strategy = Config::SaveStrategy::Rename;
    const std::string renamePath = dir + "/test_config_conf/crossy.cfg";
    commonChecks(renamePath, "rename");
    {
        Config c;
        c.setInt("volume", 3);
        c.save(renamePath);
        check(!exists(renamePath + ".tmp"), "rename: no leftover .tmp");
    }
    std::remove(renamePath.c_str());

    Config::strategy = Config::SaveStrategy::Direct;
    Config::syncFile = &recordSync;
    const std::string directPath = dir + "/test_config_direct/crossy.cfg";
    commonChecks(directPath, "direct");
    {
        synced.clear();
        Config c;
        c.setInt("highscore", 77);
        check(c.save(directPath), "direct: save");
        check(exists(directPath + ".tmp"), "direct: backup copy kept");
        check(synced.size() == 2 && synced[0] == directPath + ".tmp" && synced[1] == directPath,
              "direct: backup then file synced, in that order");

        writeText(directPath, ""); // power lost while the file was being rewritten
        Config truncated;
        check(truncated.load(directPath), "direct: truncated file loads from the backup");
        check(truncated.getInt("highscore", 0) == 77, "direct: backup values");

        std::remove(directPath.c_str());
        Config missing;
        check(missing.load(directPath), "direct: missing file loads from the backup");
        check(missing.getInt("highscore", 0) == 77, "direct: backup values when the file is gone");

        std::remove((directPath + ".tmp").c_str());
        Config nothing;
        check(!nothing.load(directPath), "direct: neither file = first start");
    }
    Config::syncFile = nullptr;
    Config::strategy = Config::SaveStrategy::Rename;

    std::printf("test_config: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
