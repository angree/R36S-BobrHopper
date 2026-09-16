// Dumps the level the PORT generates for given seeds, in the format of tools/webref/dump_map.mjs.
//   mapdump.exe --seeds 1,2,3 --out out/mapdump_port
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "engine/assets.h"
#include "engine/log.h"
#include "game/game.h"
#include "game/models.h"

using namespace cr;

static std::string fmt(double v)
{
    char b[64];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}

static std::string modelName(const Node *n)
{
    return n && n->model ? n->model->name : std::string("?");
}

int main(int argc, char **argv)
{
    std::string seeds = "1,2,3,4,5", out = "out/mapdump_port", ticksFrom;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc) seeds = argv[++i];
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--ticks-from") && i + 1 < argc) ticksFrom = argv[++i];
    }
    Manifest manifest;
    if (!loadManifest(dataDir() + "manifest.txt", manifest)) {
        logf("FATAL no manifest");
        return 2;
    }
    ModelLibrary models;
    if (!models.load(manifest, dataDir())) return 3;

    std::stringstream ss(seeds);
    std::string item;
    while (std::getline(ss, item, ',')) {
        uint32_t seed = uint32_t(std::strtoul(item.c_str(), nullptr, 10));
        Game game(models, seed);
        game.setupGame("chicken");
        game.init();
        GameMap &g = game.map();

        // match the number of frames the reference page ran before it was frozen
        int ticks = 0;
        if (!ticksFrom.empty()) {
            FILE *rf = std::fopen((ticksFrom + "/seed_" + std::to_string(seed) + ".txt").c_str(), "r");
            if (rf) {
                if (std::fscanf(rf, "ticks %d", &ticks) != 1) ticks = 0;
                std::fclose(rf);
            }
        }
        for (int t = 0; t < ticks; t++) game.step();

        std::string text;
        auto line = [&text](const std::string &s) { text += s + "\n"; };
        line("ticks " + std::to_string(ticks));
        line("rowCount " + std::to_string(g.rowCount) + " counts grass=" + std::to_string(g.grassCount) +
             " water=" + std::to_string(g.waterCount) + " road=" + std::to_string(g.roadCount) +
             " rail=" + std::to_string(g.railRoadCount));
        for (int z = 0; z < g.rowCount; z++) {
            const RowRef *row = g.getRow(z);
            if (!row) {
                line("row " + std::to_string(z) + " none");
                continue;
            }
            switch (row->type) {
            case RowType::Grass: {
                line("row " + std::to_string(z) + " grass");
                std::vector<Node *> ents = row->grass->entities;
                std::sort(ents.begin(), ents.end(), [](Node *a, Node *b) { return a->position.x < b->position.x; });
                for (Node *m : ents) line("  obstacle x=" + fmt(m->position.x) + " model=" + modelName(m));
                break;
            }
            case RowType::Water: {
                line("row " + std::to_string(z) + " water");
                std::string lily;
                for (size_t i = 0; i < row->water->lilyPadPositions.size(); i++)
                    lily += (i ? "," : "") + std::to_string(row->water->lilyPadPositions[i]);
                line("  lily " + lily);
                for (auto &e : row->water->entities)
                    line("  entity x=" + fmt(e->mesh->position.x) + " speed=" + fmt(e->speed) +
                         " width=" + std::to_string(e->width) + " box=" + fmt(e->collisionBox) +
                         " model=" + modelName(e->mesh));
                break;
            }
            case RowType::Road:
                line("row " + std::to_string(z) + " road");
                line("  lane model=" + modelName(row->road->road));
                break;
            case RowType::RailRoad:
                line("row " + std::to_string(z) + " railRoad");
                line("  train width=" + std::to_string(row->railRoad->train.width) +
                     " parts=" + std::to_string(row->railRoad->train.mesh->children.size()));
                break;
            default:
                break;
            }
        }
        for (int i = 0; i < 20; i++) {
            RoadRow &r = *g.roads[size_t(i)];
            for (auto &c : r.cars)
                line("pool road " + std::to_string(i) + " car x=" + fmt(c->mesh->position.x) + " speed=" +
                     fmt(c->speed) + " width=" + std::to_string(c->width) + " rot=" + fmt(c->mesh->rotation.y) +
                     " model=" + modelName(c->mesh));
            RailRoadRow &rr = *g.railRoads[size_t(i)];
            line("pool rail " + std::to_string(i) + " width=" + std::to_string(rr.train.width) +
                 " parts=" + std::to_string(rr.train.mesh->children.size()) + " box=" + fmt(rr.train.collisionBox));
        }

        std::string path = out + "/seed_" + std::to_string(seed) + ".txt";
        FILE *f = std::fopen(path.c_str(), "w");
        if (!f) {
            logf("cannot write %s", path.c_str());
            return 4;
        }
        std::fputs(text.c_str(), f);
        std::fclose(f);
        logf("seed %u -> %s", seed, path.c_str());
    }
    return 0;
}
