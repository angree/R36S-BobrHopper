// Model viewer: renders every model from data/manifest.txt into one contact sheet PNG, seen through
// the game camera's rotation and lit like the game. Runs in a hidden window (no focus stealing).
//   viewer.exe --out out/models_sheet.png [--cols 8] [--cell 160] [--flat data_sf2000]
// --flat draws the SF2000 flat-colour meshes (<dir>/meshes/<model>.fmesh) through the same Lambert shader, their
// colours as a palette texture, so the sheet compares pixel by pixel with the textured one (task R1.1).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/assets.h"
#include "engine/log.h"
#include "engine/platform.h"
#include "engine/png_write.h"
#include "engine/renderer.h"

using namespace cr;

int main(int argc, char **argv)
{
    std::string out = "models_sheet.png";
    int cols = 8, cell = 160;
    std::string flatDir;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--cols") && i + 1 < argc) cols = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cell") && i + 1 < argc) cell = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--flat") && i + 1 < argc) flatDir = std::string(argv[++i]) + "/";
    }
    logOpen(baseDir() + "viewer.log");

    Manifest manifest;
    if (!loadManifest(dataDir() + "manifest.txt", manifest)) {
        logf("FATAL no manifest in %s", dataDir().c_str());
        return 2;
    }
    int count = int(manifest.models.size());
    int rows = (count + cols - 1) / cols;
    int sheetW = cols * cell, sheetH = rows * cell;

    PlatformConfig cfg;
    cfg.hidden = true;
    cfg.width = sheetW;
    cfg.height = sheetH;
    Platform platform;
    if (!platform.init(cfg)) return 3;
    Renderer renderer;
    if (!renderer.init()) return 4;
    RenderTarget target;
    if (!renderer.createTarget(target, sheetW, sheetH)) return 5;
    renderer.bindTarget(&target);
    renderer.viewport(0, 0, sheetW, sheetH);
    renderer.clear(0x87 / 255.0f, 0xC6 / 255.0f, 0xFF / 255.0f);
    renderer.setLightDirection({20, 30, 0.05f});

    // game camera orientation: CrossyCamera looks at the origin from (-1, 2.8, -2.9)
    Mat4 camRot = lookAtRotation({-1, 2.8f, -2.9f}, {0, 0, 0}, {0, 1, 0});
    Vec3 back{camRot.e[8], camRot.e[9], camRot.e[10]};

    std::string names;
    int index = 0;
    for (const auto &entry : manifest.models) {
        MeshData md;
        TextureData td;
        std::string meshPath = dataDir() + "meshes/" + entry.second.mesh + ".mesh";
        std::string texPath = dataDir() + "textures/" + entry.second.texture + ".tex";
        if (!flatDir.empty()) {
            FlatMeshData fm;
            meshPath = flatDir + "meshes/" + entry.first + ".fmesh";
            if (!loadFlatMesh(meshPath, fm)) {
                logf("viewer: cannot load %s", meshPath.c_str());
                index++;
                continue;
            }
            // unindexed triangles; u picks the colour's texel of a 256x1 palette
            static const float axes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            for (const FlatMeshData::Triangle &t : fm.triangles) {
                for (uint16_t i : {t.a, t.b, t.c}) {
                    const float v[8] = {float(fm.positions[i * 3]) / 65536.0f, float(fm.positions[i * 3 + 1]) / 65536.0f,
                                        float(fm.positions[i * 3 + 2]) / 65536.0f, axes[t.axis][0], axes[t.axis][1],
                                        axes[t.axis][2], (float(t.color) + 0.5f) / 256.0f, 0.5f};
                    md.vertices.insert(md.vertices.end(), v, v + 8);
                    md.indices.push_back(uint16_t(md.indices.size()));
                }
            }
            std::memcpy(md.aabbMin, fm.aabbMin, sizeof md.aabbMin);
            std::memcpy(md.aabbMax, fm.aabbMax, sizeof md.aabbMax);
            td.width = 256;
            td.height = 1;
            td.channels = 3;
            td.pixels.assign(256 * 3, 0);
            std::copy(fm.colors.begin(), fm.colors.end(), td.pixels.begin());
        } else if (!loadMesh(meshPath, md) || !loadTexture(texPath, td)) {
            logf("viewer: cannot load %s (%s / %s)", entry.first.c_str(), meshPath.c_str(), texPath.c_str());
            index++;
            continue;
        }
        GpuMesh mesh = renderer.uploadMesh(md);
        GpuTexture tex = renderer.uploadTexture(td);

        Vec3 center = (mesh.aabbMin + mesh.aabbMax) * 0.5f;
        Mat4 world = camRot;
        Vec3 eye = center + back * 20.0f;
        world.e[12] = eye.x;
        world.e[13] = eye.y;
        world.e[14] = eye.z;
        Mat4 view = inverseRigid(world);
        double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
        for (int c = 0; c < 8; c++) {
            Vec3 p{(c & 1) ? mesh.aabbMax.x : mesh.aabbMin.x, (c & 2) ? mesh.aabbMax.y : mesh.aabbMin.y,
                   (c & 4) ? mesh.aabbMax.z : mesh.aabbMin.z};
            Vec3 v = view.transformPoint(p);
            minX = std::min(minX, v.x), maxX = std::max(maxX, v.x);
            minY = std::min(minY, v.y), maxY = std::max(maxY, v.y);
        }
        float half = std::max(maxX - minX, maxY - minY) * 0.55f;
        float cx = (minX + maxX) * 0.5f, cy = (minY + maxY) * 0.5f;
        Mat4 proj = orthographic(cx - half, cx + half, cy + half, cy - half, 0.1f, 100.0f, 1.0f);

        int col = index % cols, row = index / cols;
        renderer.viewport(col * cell, sheetH - (row + 1) * cell, cell, cell, true);
        renderer.setCamera(proj, view);
        renderer.drawLambert(mesh, tex, Mat4::identity());
        names += std::to_string(index) + " " + entry.first + "\n";
        index++;
    }

    std::vector<uint8_t> rgba;
    renderer.readPixels(sheetW, sheetH, rgba);
    bool ok = png::writeRGBA(out, sheetW, sheetH, rgba.data());
    FILE *f = std::fopen((out + ".txt").c_str(), "w");
    if (f) {
        std::fputs(names.c_str(), f);
        std::fclose(f);
    }
    logf("viewer: %d models, %d draw calls, %d triangles -> %s %s", count, renderer.stats.drawCalls,
         renderer.stats.triangles, out.c_str(), ok ? "ok" : "FAILED");
    platform.shutdown();
    return ok ? 0 : 1;
}
