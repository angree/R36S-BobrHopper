#include "render_bench.h"

#include <algorithm>
#include <cstdio>

#include "engine/assets.h"
#include "game/scene.h"
#include "game/script.h"
#include "game/settings.h"

namespace cr {

namespace {

struct SceneDef {
    const char *name;
    uint32_t seed;
    int steps;
    const char *script;
};

// build/trace_scenarios.txt: roads with cars, a railroad with a train, water with logs, sideways hops
const SceneDef kScenes[] = {
    {"car_cross_s26", 26, 90, "w20 s w20 u w20 u w10 u w10 d w10 u w10 d w10 u w10 d"},
    {"train_wait_s7", 7, 270, "w20 s w20 u w20 u w800"},
    {"hop_every20_s2", 2, 150, "w20 s w20 u w20 u w20 u w20 u w20 u w20 u"},
    {"hop_every20_s4", 4, 76, "w20 s w20 u w20 u w20 u"},
    {"sideways_s6", 6, 200, "w20 s w30 l w30 l w30 l w30 l w30 u w30 r w30 r"},
};
const int kSceneCount = int(sizeof kScenes / sizeof kScenes[0]);
const char *const kVariantNames[RenderBench::kVariants] = {"game_simple", "flat_z", "flat_noz", "tex_z", "tex_noz"};

uint8_t shadeUnit(mreal c, int32_t light)
{
    const int64_t v = (int64_t(c.v) * light * 255 + (int64_t(1) << 31)) >> 32;
    return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
}

// renderer_sw.cpp faceGradient: the exact depth gradient of the faces of axis k
bool faceGradient(const sw::Mat34 &t, int k, sw::DepthGradient &out)
{
    const int u = k == 0 ? 1 : 0, v = k == 2 ? 1 : 2;
    const int64_t t0u = t.m[u], t0v = t.m[v], t1u = t.m[4 + u], t1v = t.m[4 + v], t2u = t.m[8 + u], t2v = t.m[8 + v];
    int64_t det = t0u * t1v - t0v * t1u;
    int64_t nx = t2u * t1v - t2v * t1u;
    int64_t ny = t0u * t2v - t0v * t2u;
    const int64_t limit = int64_t(1) << 44;
    while (nx >= limit || nx <= -limit || ny >= limit || ny <= -limit) {
        det /= 2;
        nx /= 2;
        ny /= 2;
    }
    if (det == 0) return false;
    const int64_t gx = nx * (int64_t(1) << (sw::kDepthUnitBits + 8)) / det;
    const int64_t gy = ny * (int64_t(1) << (sw::kDepthUnitBits + 8)) / det;
    const int64_t maxG = int64_t(1) << 23;
    if (gx > maxG || gx < -maxG || gy > maxG || gy < -maxG) return false;
    out.dzdx = int32_t(gx);
    out.dzdy = int32_t(gy);
    return true;
}

real maxScale(const Mat4 &m)
{
    const real sx = m.e[0] * m.e[0] + m.e[1] * m.e[1] + m.e[2] * m.e[2];
    const real sy = m.e[4] * m.e[4] + m.e[5] * m.e[5] + m.e[6] * m.e[6];
    const real sz = m.e[8] * m.e[8] + m.e[9] * m.e[9] + m.e[10] * m.e[10];
    real s = sx > sy ? sx : sy;
    if (sz > s) s = sz;
    return rsqrt(s);
}

struct FlatSpan {
    const sw::RasterTarget &t;
    uint16_t color;
    bool depth;
    void operator()(int y, int x0, int x1, int32_t z, int32_t dz)
    {
        if (t.counters) t.counters->spans++, t.counters->pixels += x1 - x0;
        uint16_t *p = t.color + y * t.rowStride();
        if (!depth) {
            for (int x = x0; x < x1; x++) p[x] = color;
            return;
        }
        uint16_t *d = t.depth + y * t.rowStride();
        for (int x = x0; x < x1; x++, z += dz) {
            const int32_t zi = z >> 8;
            if (zi <= int32_t(d[x])) {
                p[x] = color;
                d[x] = uint16_t(zi < 0 ? 0 : zi);
            }
        }
    }
};

// texels (16.16) stepped per pixel from the triangle's plane, nearest sampling through a shaded palette
struct TexSpan {
    const sw::RasterTarget &t;
    const uint8_t *tex;
    int tw, th;
    const uint16_t *palette;
    bool depth;
    int32_t ax, ay; // vertex a, 28.4
    int64_t sa, ta, dsdx, dsdy, dtdx, dtdy;
    void operator()(int y, int x0, int x1, int32_t z, int32_t dz)
    {
        if (t.counters) t.counters->spans++, t.counters->pixels += x1 - x0;
        const int64_t xc = int64_t(x0) * sw::kSubpixelOne + sw::kSubpixelOne / 2 - ax;
        const int64_t yc = int64_t(y) * sw::kSubpixelOne + sw::kSubpixelOne / 2 - ay;
        int32_t s = int32_t(sa + ((dsdx * xc + dsdy * yc) >> sw::kSubpixelBits));
        int32_t tt = int32_t(ta + ((dtdx * xc + dtdy * yc) >> sw::kSubpixelBits));
        const int32_t ds = int32_t(dsdx), dt = int32_t(dtdx);
        uint16_t *p = t.color + y * t.rowStride();
        uint16_t *d = depth ? t.depth + y * t.rowStride() : nullptr;
        const int maxX = tw - 1, maxY = th - 1;
        for (int x = x0; x < x1; x++, s += ds, tt += dt, z += dz) {
            if (d) {
                const int32_t zi = z >> 8;
                if (zi > int32_t(d[x])) continue;
                d[x] = uint16_t(zi < 0 ? 0 : zi);
            }
            int tx = s >> 16, ty = tt >> 16;
            tx = tx < 0 ? 0 : tx > maxX ? maxX : tx;
            ty = ty < 0 ? 0 : ty > maxY ? maxY : ty;
            p[x] = palette[tex[ty * tw + tx]];
        }
    }
};

const sw::DepthGradient kNoGradient{0, 0};

} // namespace

bool loadTexMesh(const std::string &path, TexMesh &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> buf;
    uint8_t chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) buf.insert(buf.end(), chunk, chunk + n);
    std::fclose(f);
    size_t at = 0;
    auto need = [&](size_t k) { return at + k <= buf.size(); };
    auto u32 = [&]() { uint32_t v = uint32_t(buf[at]) | uint32_t(buf[at + 1]) << 8 | uint32_t(buf[at + 2]) << 16 | uint32_t(buf[at + 3]) << 24; at += 4; return v; };
    auto u16 = [&]() { uint16_t v = uint16_t(buf[at] | buf[at + 1] << 8); at += 2; return v; };
    if (!need(20) || buf[0] != 'C' || buf[1] != 'R' || buf[2] != 'T' || buf[3] != 'M') return false;
    at = 4;
    const uint32_t vc = u32(), tc = u32();
    out.texW = u16();
    out.texH = u16();
    const int cc = u16();
    u16();
    const size_t texels = size_t(out.texW) * size_t(out.texH);
    if (vc > 65536 || cc > 256 || !need(size_t(cc) * 3 + texels + size_t(vc) * 20 + size_t(tc) * 8)) return false;
    out.colours.assign(buf.begin() + long(at), buf.begin() + long(at + size_t(cc) * 3));
    at += size_t(cc) * 3;
    out.texture.assign(buf.begin() + long(at), buf.begin() + long(at + texels));
    at += texels;
    out.positions.resize(size_t(vc) * 3);
    out.texels.resize(size_t(vc) * 2);
    for (uint32_t i = 0; i < vc; i++) {
        for (int k = 0; k < 3; k++) out.positions[size_t(i) * 3 + size_t(k)] = int32_t(u32());
        for (int k = 0; k < 2; k++) out.texels[size_t(i) * 2 + size_t(k)] = int32_t(u32());
    }
    out.triangles.resize(tc);
    for (TexMesh::Triangle &t : out.triangles) {
        t.a = u16();
        t.b = u16();
        t.c = u16();
        t.axis = buf[at];
        at += 2;
        if (t.a >= vc || t.b >= vc || t.c >= vc || t.axis > 5) return false;
    }
    for (uint8_t idx : out.texture)
        if (idx >= cc) return false;
    int next = 0;
    for (int a = 0; a <= 6; a++) {
        while (next < int(tc) && out.triangles[size_t(next)].axis < a) next++;
        out.axisStart[a] = next;
    }
    return true;
}

bool RenderBench::init(ModelLibrary &models, const Manifest &manifest, const std::string &dataDir,
                       const std::string &version)
{
    models_ = &models;
    version_ = version;
    for (const auto &entry : manifest.models) {
        Model *model = models.find(entry.first);
        if (!model) continue;
        std::unique_ptr<TexMesh> m(new TexMesh());
        if (loadTexMesh(dataDir + "meshes/" + entry.first + ".tmesh", *m)) texMeshes_.push_back({model, std::move(m)});
    }
    cells_.assign(size_t(kSceneCount), std::vector<Cell>(size_t(kVariants)));
    return true;
}

const TexMesh *RenderBench::texMeshFor(const Model *model) const
{
    for (const auto &e : texMeshes_)
        if (e.first == model) return e.second.get();
    return nullptr;
}

void RenderBench::loadScene(int index)
{
    const SceneDef &s = kScenes[index];
    game_.reset(new Game(*models_, s.seed));
    game_->setupGame("chicken");
    game_->init();
    game_->tickEngineOnly();
    game_->takeSounds();
    InputScript input;
    input.parse(s.script);
    for (int k = 0; k < s.steps; k++) {
        input.apply(*game_);
        game_->step();
        game_->takeSounds();
        game_->endFrame();
    }
}

void RenderBench::beginVertices(int count)
{
    verts_.resize(size_t(count));
    stamps_.resize(size_t(count), 0);
    if (++stamp_ == 0) {
        std::fill(stamps_.begin(), stamps_.end(), 0u);
        stamp_ = 1;
    }
}

void RenderBench::collect(Node *node)
{
    if (!node->visible) return;
    const Model *model = node->model;
    const bool hasMesh = model && model->mesh;
    if (hasMesh || node->shape != Shape::None) {
        const Vec3 mn = hasMesh ? model->aabbMin : Vec3{real(-0.5), real(-0.5), real(-0.5)};
        const Vec3 mx = hasMesh ? model->aabbMax : Vec3{real(0.5), real(0.5), real(0.5)};
        Mat4 world = node->world;
        if (node->shape != Shape::None) world = mulAffine(world, composeEuler({0, 0, 0}, {0, 0, 0}, node->shapeSize));
        world.e[12] -= origin_.x;
        world.e[13] -= origin_.y;
        world.e[14] -= origin_.z;
        const Vec3 center = world.transformPoint((mn + mx) * real(0.5));
        const real radius = length(mx - mn) * real(0.5) * maxScale(world);
        const Vec3 clip = viewProjRelative_.transformPoint(center);
        const real margin = 1.5;
        const bool outside = rabs(clip.x) > real(1) + radius * rabs(projection_.e[0]) * margin ||
                             rabs(clip.y) > real(1) + radius * rabs(projection_.e[5]) * margin;
        if (!outside) {
            const int32_t *m = screen_.m;
            Item it;
            it.node = node;
            it.world = world;
            it.depthKey = int64_t(m[8]) * center.x.v + int64_t(m[9]) * center.y.v + int64_t(m[10]) * center.z.v +
                          (int64_t(m[11]) << 16);
            it.floor = hasMesh && model->receiveShadow && !model->castShadow;
            items_.push_back(it);
        }
    }
    for (Node *c : node->children) collect(c);
}

void RenderBench::drawFlat(const sw::RasterTarget &rt, const FlatMeshData &d, const int *axisStart, const Mat4 &world,
                           const Vec3 *color, bool doubleSided, bool depth)
{
    const sw::Mat34 t = sw::modelToScreen(screen_, world);
    beginVertices(d.vertexCount());
    int32_t light[6];
    sw::axisBrightness(world, light_, light);
    const int colors = color ? 1 : d.colorCount();
    shades_.resize(size_t(colors) * 6);
    for (int c = 0; c < colors; c++)
        for (int axis = 0; axis < 6; axis++) {
            uint8_t r, g, b;
            if (color) {
                r = shadeUnit(color->x, light[axis]);
                g = shadeUnit(color->y, light[axis]);
                b = shadeUnit(color->z, light[axis]);
            } else {
                const uint8_t *rgb = &d.colors[size_t(c) * 3];
                r = sw::shade8(rgb[0], light[axis]);
                g = sw::shade8(rgb[1], light[axis]);
                b = sw::shade8(rgb[2], light[axis]);
            }
            shades_[size_t(c * 6 + axis)] = sw::rgb565(r, g, b);
        }
    sw::DepthGradient gradients[3];
    bool gradientOk[3] = {false, false, false};
    if (depth)
        for (int k = 0; k < 3; k++) gradientOk[k] = faceGradient(t, k, gradients[k]);
    bool frontAxis[6];
    for (int k = 0; k < 3; k++) {
        const int u = k == 0 ? 1 : 0, v = k == 2 ? 1 : 2;
        const int64_t det = int64_t(t.m[u]) * t.m[4 + v] - int64_t(t.m[v]) * t.m[4 + u];
        const int64_t areaPlus = k == 1 ? -det : det;
        frontAxis[k * 2] = areaPlus < 0;
        frontAxis[k * 2 + 1] = areaPlus > 0;
    }
    const bool ranges = axisStart[6] >= 0;
    const int32_t *pos = d.positions.data();
    for (int group = 0; group < (ranges ? 6 : 1); group++) {
        if (ranges && !doubleSided && !frontAxis[group]) continue;
        const size_t first = ranges ? size_t(axisStart[group]) : 0;
        const size_t last = ranges ? size_t(axisStart[group + 1]) : d.triangles.size();
        for (size_t i = first; i < last; i++) {
            const FlatMeshData::Triangle &tri = d.triangles[i];
            if (!doubleSided && !frontAxis[tri.axis]) continue;
            const sw::ScreenVertex *v3[3];
            const uint16_t idx[3] = {tri.a, tri.b, tri.c};
            for (int k = 0; k < 3; k++) {
                const uint16_t n = idx[k];
                if (stamps_[n] != stamp_) {
                    stamps_[n] = stamp_;
                    verts_[n] = sw::toScreen(sw::transformQ32(t, pos[n * 3], pos[n * 3 + 1], pos[n * 3 + 2]));
                }
                v3[k] = &verts_[n];
            }
            const sw::ScreenVertex &a = *v3[0], &b = *v3[1], &c = *v3[2];
            const int64_t area = int64_t(b.x - a.x) * (c.y - a.y) - int64_t(c.x - a.x) * (b.y - a.y);
            if (area == 0) continue;
            const bool front = area < 0;
            if (!front && !doubleSided) continue;
            const int axis = front ? tri.axis : tri.axis ^ 1;
            const uint16_t shade = shades_[size_t((color ? 0 : tri.color) * 6 + axis)];
            const int k = tri.axis >> 1;
            FlatSpan span{rt, shade, depth};
            sw::rasterTriangle(rt, a, b, c, false, span,
                               depth ? (gradientOk[k] ? &gradients[k] : nullptr) : &kNoGradient);
            trianglesDrawn_++;
        }
    }
}

void RenderBench::drawTex(const sw::RasterTarget &rt, const TexMesh &m, const Mat4 &world, bool depth)
{
    const sw::Mat34 t = sw::modelToScreen(screen_, world);
    beginVertices(m.vertexCount());
    int32_t light[6];
    sw::axisBrightness(world, light_, light);
    const int colours = m.colourCount();
    shades_.resize(size_t(colours) * 6);
    for (int axis = 0; axis < 6; axis++)
        for (int c = 0; c < colours; c++) {
            const uint8_t *rgb = &m.colours[size_t(c) * 3];
            shades_[size_t(axis * colours + c)] =
                sw::rgb565(sw::shade8(rgb[0], light[axis]), sw::shade8(rgb[1], light[axis]), sw::shade8(rgb[2], light[axis]));
        }
    sw::DepthGradient gradients[3];
    bool gradientOk[3] = {false, false, false};
    if (depth)
        for (int k = 0; k < 3; k++) gradientOk[k] = faceGradient(t, k, gradients[k]);
    bool frontAxis[6];
    for (int k = 0; k < 3; k++) {
        const int u = k == 0 ? 1 : 0, v = k == 2 ? 1 : 2;
        const int64_t det = int64_t(t.m[u]) * t.m[4 + v] - int64_t(t.m[v]) * t.m[4 + u];
        const int64_t areaPlus = k == 1 ? -det : det;
        frontAxis[k * 2] = areaPlus < 0;
        frontAxis[k * 2 + 1] = areaPlus > 0;
    }
    const int32_t *pos = m.positions.data();
    const int32_t *uv = m.texels.data();
    for (int group = 0; group < 6; group++) {
        if (!frontAxis[group]) continue;
        for (int i = m.axisStart[group]; i < m.axisStart[group + 1]; i++) {
            const TexMesh::Triangle &tri = m.triangles[size_t(i)];
            const sw::ScreenVertex *v3[3];
            const uint16_t idx[3] = {tri.a, tri.b, tri.c};
            for (int k = 0; k < 3; k++) {
                const uint16_t n = idx[k];
                if (stamps_[n] != stamp_) {
                    stamps_[n] = stamp_;
                    verts_[n] = sw::toScreen(sw::transformQ32(t, pos[n * 3], pos[n * 3 + 1], pos[n * 3 + 2]));
                }
                v3[k] = &verts_[n];
            }
            const sw::ScreenVertex &a = *v3[0], &b = *v3[1], &c = *v3[2];
            const int64_t dx1 = b.x - a.x, dy1 = b.y - a.y, dx2 = c.x - a.x, dy2 = c.y - a.y;
            const int64_t area = dx1 * dy2 - dx2 * dy1;
            if (area >= 0) continue; // edge-on, or turned away (counter-clockwise in GL = clockwise on screen)
            const int64_t sa = uv[tri.a * 2], ta = uv[tri.a * 2 + 1];
            const int64_t ds1 = uv[tri.b * 2] - sa, ds2 = uv[tri.c * 2] - sa;
            const int64_t dt1 = uv[tri.b * 2 + 1] - ta, dt2 = uv[tri.c * 2 + 1] - ta;
            TexSpan span{rt, m.texture.data(), m.texW, m.texH, &shades_[size_t(tri.axis * colours)], depth, a.x, a.y,
                         sa, ta,
                         ((ds1 * dy2 - ds2 * dy1) * sw::kSubpixelOne) / area, ((ds2 * dx1 - ds1 * dx2) * sw::kSubpixelOne) / area,
                         ((dt1 * dy2 - dt2 * dy1) * sw::kSubpixelOne) / area, ((dt2 * dx1 - dt1 * dx2) * sw::kSubpixelOne) / area};
            const int k = tri.axis >> 1;
            sw::rasterTriangle(rt, a, b, c, false, span,
                               depth ? (gradientOk[k] ? &gradients[k] : nullptr) : &kNoGradient);
            trianglesDrawn_++;
        }
    }
}

void RenderBench::drawItem(const sw::RasterTarget &rt, Renderer &renderer, const Item &item, bool textured, bool depth)
{
    const Node *node = item.node;
    if (node->shape != Shape::None) {
        const bool plane = node->shape == Shape::Plane;
        const GpuMesh &mesh = plane ? renderer.unitPlane : renderer.unitBox;
        drawFlat(rt, *mesh.data, mesh.axisStart, item.world, &node->shapeColor, plane, depth);
        return;
    }
    const Model *model = node->model;
    const TexMesh *tm = textured ? texMeshFor(model) : nullptr;
    if (tm) drawTex(rt, *tm, item.world, depth);
    else drawFlat(rt, *model->mesh->data, model->mesh->axisStart, item.world, nullptr, false, depth);
}

void RenderBench::drawVariant(Renderer &renderer, SceneRenderer &scene, mreal viewScale, int variant)
{
    const RenderTarget &target = renderer.target();
    const int width = target.width, height = target.height;
    static const mreal skyR = 0x87 / 255.0f, skyG = 0xC6 / 255.0f, skyB = 0xFF / 255.0f;
    if (variant == 0) {
        // the game's own frame (libretro_core.cpp GameApp::render, without the HUD)
        renderer.bindTarget(nullptr);
        renderer.clear(skyR, skyG, skyB);
        renderer.resetStats();
        const ShadowMode old = scene.shadowMode;
        scene.shadowMode = ShadowMode::Simple;
        scene.render(renderer, *game_, width, height, viewScale);
        scene.shadowMode = old;
        trianglesDrawn_ = renderer.stats.trianglesDrawn;
        counters_.pixels = renderer.stats.pixels;
        return;
    }
    const bool textured = variant >= 3, depth = variant == 1 || variant == 3;
    RenderTarget &mutableTarget = const_cast<RenderTarget &>(target);
    sw::RasterTarget rt;
    rt.width = width;
    rt.height = height;
    rt.stride = width;
    rt.color = mutableTarget.color.data();
    rt.depth = depth ? mutableTarget.depth.data() : nullptr;
    rt.counters = &counters_;
    counters_ = sw::RasterCounters();
    trianglesDrawn_ = 0;

    sw::clearTarget(rt, sw::rgb565(0x87, 0xC6, 0xFF), uint16_t(sw::kDepthMax));
    updateWorld(game_->sceneRoot());
    scene.setupCamera(*game_, width, height, viewScale);
    projection_ = scene.projection;
    viewRelative_ = inverseRigid(lookAtRotation({-1, real(2.8), real(-2.9)}, {0, 0, 0}, {0, 1, 0}));
    viewProjRelative_ = projection_ * viewRelative_;
    origin_ = game_->cameraPosition();
    screen_ = sw::screenMatrix(projection_, viewRelative_, width, height);
    light_ = normalize({settings::lightX, settings::lightY, settings::lightZ});

    items_.clear();
    collect(game_->sceneRoot());
    if (!depth)
        std::stable_sort(items_.begin(), items_.end(), [](const Item &a, const Item &b) {
            if (a.floor != b.floor) return a.floor; // floors first: nothing stands below them
            return a.depthKey > b.depthKey;         // then far to near
        });
    for (const Item &it : items_) drawItem(rt, renderer, it, textured, depth);
}

std::string RenderBench::report() const
{
    std::string out;
    char buf[200];
    snprintf(buf, sizeof buf, "BobrHopper %s renderbench\nrounds_complete %d scene %d variant %d\n", version_.c_str(),
             round_, scene_, variant_);
    out += buf;
    snprintf(buf, sizeof buf, "tex_meshes %d frames_per_variant %d (+1 warm-up)\n", int(texMeshes_.size()),
             kFramesPerVariant);
    out += buf;
    out += "# scene variant frames ms us_per_frame tris_per_frame pixels_per_frame\n";
    long sumUs[kVariants] = {0, 0, 0, 0, 0};
    int counted[kVariants] = {0, 0, 0, 0, 0};
    for (int s = 0; s < kSceneCount; s++)
        for (int v = 0; v < kVariants; v++) {
            const Cell &c = cells_[size_t(s)][size_t(v)];
            if (c.frames == 0) continue;
            const long us = long(int64_t(c.ms) * 1000 / c.frames);
            snprintf(buf, sizeof buf, "%s %s %ld %ld %ld %ld %ld\n", kScenes[s].name, kVariantNames[v], c.frames, c.ms,
                     us, long(c.triangles / c.frames), long(c.pixels / c.frames));
            out += buf;
            sumUs[v] += us;
            counted[v]++;
        }
    out += "# average over the scenes measured, us per frame\n";
    for (int v = 0; v < kVariants; v++) {
        if (!counted[v]) continue;
        snprintf(buf, sizeof buf, "avg %s %ld (%d scenes)\n", kVariantNames[v], sumUs[v] / counted[v], counted[v]);
        out += buf;
    }
    return out;
}

RenderBench::Frame RenderBench::step(Renderer &renderer, SceneRenderer &scene, mreal viewScale, uint32_t (*clockMs)())
{
    Frame f;
    if (!game_) loadScene(scene_);
    const uint32_t t0 = clockMs();
    drawVariant(renderer, scene, viewScale, variant_);
    const uint32_t t1 = clockMs();
    if (frameInVariant_ > 0) {
        Cell &c = cells_[size_t(scene_)][size_t(variant_)];
        c.frames++;
        c.ms += long(t1 - t0);
        c.triangles += trianglesDrawn_;
        c.pixels += counters_.pixels;
    }
    char buf[96];
    snprintf(buf, sizeof buf, "RENDER BENCH %s  ROUND %d", version_.c_str(), round_ + 1);
    f.line1 = buf;
    snprintf(buf, sizeof buf, "%s  %s  %d/%d", kScenes[scene_].name, kVariantNames[variant_], frameInVariant_,
             kFramesPerVariant);
    f.line2 = buf;
    snprintf(buf, sizeof buf, "%u ms  %ld tris  %ld px", unsigned(t1 - t0), trianglesDrawn_, long(counters_.pixels));
    f.line3 = buf;
    if (++frameInVariant_ > kFramesPerVariant) {
        frameInVariant_ = 0;
        if (++variant_ == kVariants) {
            variant_ = 0;
            game_.reset();
            if (++scene_ == kSceneCount) {
                scene_ = 0;
                round_++;
            }
        }
        f.writeResults = true;
        f.results = report();
    }
    return f;
}

} // namespace cr
