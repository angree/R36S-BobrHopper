// SF2000 render benchmark (ROM "RenderBench"): the same game frames drawn in several ways to decide how the software
// renderer should draw the game. The game itself is not changed by it. Variants:
//   game_simple  the game's SceneRenderer as it plays (flat colour meshes, depth buffer, SIMPLE shadows)
//   flat_z       flat colour meshes (.fmesh), depth buffer, no shadows
//   flat_noz     flat colour meshes without a depth buffer: floors, then everything else, each far to near
//   tex_z        textured meshes (.tmesh: the GLES triangles, palette textures), depth buffer
//   tex_noz      textured meshes without a depth buffer (the order of flat_noz)
// Every variant draws 1 warm-up and kFramesPerVariant timed frames of each scene; the rounds repeat until the console is
// switched off, and the results are rewritten after every variant (renderbench.txt).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/renderer.h"
#include "game/game.h"
#include "game/models.h"
#include "game/scene_render.h"
#include "sw/raster.h"

namespace cr {

struct Manifest;

struct TexMesh {
    struct Triangle {
        uint16_t a, b, c;
        uint8_t axis;
    };
    std::vector<int32_t> positions; // x y z per vertex, 16.16
    std::vector<int32_t> texels;    // s t per vertex, texels of the small texture, 16.16
    std::vector<Triangle> triangles;
    int axisStart[7] = {0, 0, 0, 0, 0, 0, 0};
    int texW = 0, texH = 0;
    std::vector<uint8_t> texture; // colour indices, rows top first
    std::vector<uint8_t> colours; // r g b
    int vertexCount() const { return int(positions.size() / 3); }
    int colourCount() const { return int(colours.size() / 3); }
};

bool loadTexMesh(const std::string &path, TexMesh &out);

class RenderBench {
public:
    static const int kVariants = 5;
    static const int kFramesPerVariant = 40;

    bool init(ModelLibrary &models, const Manifest &manifest, const std::string &dataDir, const std::string &version);
    int texMeshes() const { return int(texMeshes_.size()); }

    struct Frame {
        std::string line1, line2, line3;
        bool writeResults = false;
        std::string results;
    };
    // draws the next benchmark frame into the renderer's default target; clockMs measures the drawing
    Frame step(Renderer &renderer, SceneRenderer &scene, mreal viewScale, uint32_t (*clockMs)());

private:
    struct Item {
        Node *node;
        Mat4 world;
        int64_t depthKey; // distance in front of the camera, Q32 world units (larger = farther)
        bool floor;
    };
    struct Cell {
        long frames = 0;
        long ms = 0;
        int64_t triangles = 0, pixels = 0;
    };

    void loadScene(int index);
    void drawVariant(Renderer &renderer, SceneRenderer &scene, mreal viewScale, int variant);
    void collect(Node *node);
    void drawItem(const sw::RasterTarget &rt, Renderer &renderer, const Item &item, bool textured, bool depth);
    void drawFlat(const sw::RasterTarget &rt, const FlatMeshData &d, const int *axisStart, const Mat4 &world,
                  const Vec3 *color, bool doubleSided, bool depth);
    void drawTex(const sw::RasterTarget &rt, const TexMesh &m, const Mat4 &world, bool depth);
    void beginVertices(int count);
    std::string report() const;

    ModelLibrary *models_ = nullptr;
    std::string version_;
    std::vector<std::pair<const Model *, std::unique_ptr<TexMesh>>> texMeshes_;
    const TexMesh *texMeshFor(const Model *model) const;

    std::unique_ptr<Game> game_;
    int scene_ = 0, variant_ = 0, frameInVariant_ = 0, round_ = 0;
    std::vector<std::vector<Cell>> cells_;

    // camera of the frame being drawn (SceneRenderer::setupCamera)
    Mat4 projection_ = Mat4::identity(), viewRelative_ = Mat4::identity(), viewProjRelative_ = Mat4::identity();
    Vec3 origin_, light_;
    sw::Mat34 screen_{};
    std::vector<Item> items_;
    std::vector<sw::ScreenVertex> verts_;
    std::vector<uint32_t> stamps_;
    uint32_t stamp_ = 0;
    std::vector<uint16_t> shades_;
    sw::RasterCounters counters_;
    long trianglesDrawn_ = 0;
};

} // namespace cr
