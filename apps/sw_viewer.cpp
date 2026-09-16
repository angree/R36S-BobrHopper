// The model contact sheet of apps/viewer.cpp drawn by the SF2000 software renderer (CR_FIXED): the same cells, camera
// and light, so the PNG compares with `viewer --flat data_sf2000` pixel by pixel (task R1.4).
//   sw_viewer.exe --out out/check/sw_viewer/sheet.png [--data data_sf2000] [--cols 8] [--cell 256]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/assets.h"
#include "engine/log.h"
#include "engine/png_write.h"
#include "engine/renderer.h"

using namespace cr;

int main(int argc, char **argv)
{
    std::string out = "sw_models_sheet.png", data = "data_sf2000";
    int cols = 8, cell = 256;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!std::strcmp(argv[i], "--cols") && i + 1 < argc) cols = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cell") && i + 1 < argc) cell = std::atoi(argv[++i]);
    }
    data += "/";
    Manifest manifest;
    if (!loadManifest(data + "manifest.txt", manifest)) {
        std::fprintf(stderr, "no manifest in %s\n", data.c_str());
        return 2;
    }
    const int count = int(manifest.models.size());
    const int rows = (count + cols - 1) / cols;
    const int sheetW = cols * cell, sheetH = rows * cell;
    // the sky colour everywhere first, like the GLES viewer's clear (the last row has empty cells)
    std::vector<uint8_t> sheet(size_t(sheetW) * size_t(sheetH) * 4);
    for (size_t i = 0; i < sheet.size(); i += 4) sheet[i] = 0x87, sheet[i + 1] = 0xC6, sheet[i + 2] = 0xFF, sheet[i + 3] = 255;

    Renderer renderer;
    renderer.init(cell, cell);
    renderer.setLightDirection({20, 30, Fixed(0.05)});
    const Mat4 camRot = lookAtRotation({-1, Fixed(2.8), Fixed(-2.9)}, {0, 0, 0}, {0, 1, 0});
    const Vec3 back{camRot.e[8], camRot.e[9], camRot.e[10]};

    int index = 0, drawn = 0;
    for (const auto &entry : manifest.models) {
        FlatMeshData fm;
        const std::string path = data + "meshes/" + entry.first + ".fmesh";
        if (!loadFlatMesh(path, fm)) {
            std::fprintf(stderr, "cannot load %s\n", path.c_str());
            index++;
            continue;
        }
        GpuMesh mesh = renderer.uploadMesh(fm);
        // viewer.cpp's framing: the camera 20 units back from the box centre, the box's view-space extent * 1.1
        const Vec3 center = (mesh.aabbMin + mesh.aabbMax) * Fixed(0.5);
        Mat4 world = camRot;
        const Vec3 eye = center + back * real(20);
        world.e[12] = eye.x;
        world.e[13] = eye.y;
        world.e[14] = eye.z;
        const Mat4 view = inverseRigid(world);
        real minX = 1000, maxX = -1000, minY = 1000, maxY = -1000;
        for (int c = 0; c < 8; c++) {
            const Vec3 p{(c & 1) ? mesh.aabbMax.x : mesh.aabbMin.x, (c & 2) ? mesh.aabbMax.y : mesh.aabbMin.y,
                         (c & 4) ? mesh.aabbMax.z : mesh.aabbMin.z};
            const Vec3 v = view.transformPoint(p);
            if (v.x < minX) minX = v.x;
            if (v.x > maxX) maxX = v.x;
            if (v.y < minY) minY = v.y;
            if (v.y > maxY) maxY = v.y;
        }
        const real half = (maxX - minX > maxY - minY ? maxX - minX : maxY - minY) * Fixed(0.55);
        const real cx = (minX + maxX) * Fixed(0.5), cy = (minY + maxY) * Fixed(0.5);
        const Mat4 proj = orthographic(cx - half, cx + half, cy + half, cy - half, Fixed(0.1), 100, 1);

        renderer.bindTarget(nullptr);
        renderer.clear(Fixed(0x87 / 255.0), Fixed(0xC6 / 255.0), Fixed(0xFF / 255.0));
        renderer.setCamera(proj, view);
        renderer.drawLambert(mesh, GpuTexture(), Mat4::identity());
        std::vector<uint8_t> rgba;
        renderer.readPixels(cell, cell, rgba);
        const int col = index % cols, row = index / cols;
        for (int y = 0; y < cell; y++)
            std::memcpy(&sheet[(size_t(row * cell + y) * size_t(sheetW) + size_t(col * cell)) * 4],
                        &rgba[size_t(y) * size_t(cell) * 4], size_t(cell) * 4);
        renderer.releaseMesh(mesh);
        drawn++;
        index++;
    }
    const bool ok = png::writeRGBA(out, sheetW, sheetH, sheet.data());
    std::printf("sw_viewer: %d models, %d draw calls, %d triangles, %d drawn -> %s %s\n", drawn, renderer.stats.drawCalls,
                renderer.stats.triangles, renderer.stats.trianglesDrawn, out.c_str(), ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
