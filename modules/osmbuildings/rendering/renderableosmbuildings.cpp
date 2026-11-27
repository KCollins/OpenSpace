#include <modules/osmbuildings/rendering/renderableosmbuildings.h>

#include <openspace/documentation/documentation.h>
#include <openspace/engine/globals.h>
#include <openspace/filesystem/filesystem.h>
#include <openspace/logging/logmanager.h>
#include <openspace/opengl/programobject.h>
#include <openspace/rendering/renderengine.h>
#include <openspace/util/updatestructures.h>
#include <glm/gtc/matrix_transform.hpp>
#include <curl/curl.h>
#include <cmath>
#include <cstring>
#include <format>

using namespace gl;

namespace {
    constexpr std::string_view _loggerCat = "RenderableOsmBuildings";
    constexpr double EarthRadius = 6378137.0;
    constexpr int TileZoom = 15;

    constexpr openspace::StringPropertyInfo ServerUrlInfo = {
        .identifier = "ServerUrl",
        .name = "Server URL",
        .description = "Base URL of the local Python OSM mesh server.",
        .visibility = openspace::Visibility::AdvancedUser
    };

    constexpr openspace::FloatPropertyInfo ActivationAltitudeInfo = {
        .identifier = "ActivationAltitude",
        .name = "Activation Altitude",
        .description = "Altitude in meters below which 3D buildings stream and render.",
        .visibility = openspace::Visibility::User,
        .defaults = {
            .value = 15000.f,
            .minMax = { .min = 500.f, .max = 100000.f }
        }
    };

    constexpr openspace::FloatPropertyInfo OpacityInfo = {
        .identifier = "Opacity",
        .name = "Opacity",
        .description = "Opacity of the extruded 3D buildings.",
        .visibility = openspace::Visibility::NoviceUser,
        .defaults = {
            .value = 0.95f,
            .minMax = { .min = 0.0f, .max = 1.0f }
        }
    };

    struct [[codegen::Dictionary(RenderableOsmBuildings)]] Parameters {
        // [[codegen::verbatim(ServerUrlInfo.description)]]
        std::optional<std::string> serverUrl;

        // [[codegen::verbatim(ActivationAltitudeInfo.description)]]
        std::optional<float> activationAltitude [[codegen::greater(0.0)]];

        // [[codegen::verbatim(OpacityInfo.description)]]
        std::optional<float> opacity [[codegen::inrange(0.0, 1.0)]];
    };

    size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        const size_t total = size * nmemb;
        auto* buf = static_cast<std::vector<uint8_t>*>(userp);
        const auto* bytes = static_cast<const uint8_t*>(contents);
        buf->insert(buf->end(), bytes, bytes + total);
        return total;
    }

    std::pair<int, int> latLonToTile(double latDeg, double lonDeg, int z) {
        const double n = std::pow(2.0, z);
        const int x = static_cast<int>(std::floor((lonDeg + 180.0) / 360.0 * n));
        const double latRad = glm::radians(latDeg);
        const int y = static_cast<int>(std::floor(
            (1.0 - std::asinh(std::tan(latRad)) / glm::pi<double>()) / 2.0 * n
        ));
        return { x, y };
    }
} // namespace

#include "renderableosmbuildings_codegen.cpp"

namespace openspace {

Documentation RenderableOsmBuildings::Documentation() {
    return codegen::doc<Parameters>("osmbuildings_renderableosmbuildings");
}

RenderableOsmBuildings::RenderableOsmBuildings(const Dictionary& dictionary)
    : Renderable(dictionary)
    , _serverUrl(ServerUrlInfo)
    , _activationAltitude(ActivationAltitudeInfo)
    , _buildingOpacity(OpacityInfo)
{
    const Parameters p = codegen::bake<Parameters>(dictionary);

    _serverUrl = p.serverUrl.value_or("http://127.0.0.1:8765");
    addProperty(_serverUrl);

    _activationAltitude = p.activationAltitude.value_or(_activationAltitude);
    addProperty(_activationAltitude);

    _buildingOpacity = p.opacity.value_or(_buildingOpacity);
    addProperty(_buildingOpacity);
}

void RenderableOsmBuildings::initializeGL() {
    _program = global::renderEngine->buildRenderProgram(
        "OsmBuildingsProgram",
        absPath("${MODULE_OSMBUILDINGS}/shaders/buildings_vs.glsl"),
        absPath("${MODULE_OSMBUILDINGS}/shaders/buildings_fs.glsl")
    );
}

void RenderableOsmBuildings::deinitializeGL() {
    for (auto& [key, tile] : _gpuTiles) {
        glDeleteVertexArrays(1, &tile.vao);
        glDeleteBuffers(1, &tile.vbo);
        glDeleteBuffers(1, &tile.ibo);
    }
    _gpuTiles.clear();

    if (_program) {
        global::renderEngine->removeRenderProgram(_program.get());
        _program = nullptr;
    }
}

std::optional<RenderableOsmBuildings::TileMeshData> RenderableOsmBuildings::fetchTileBinary(
    const std::string& baseUrl, int z, int x, int y)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        return std::nullopt;
    }

    const std::string url = std::format("{}/tile/{}/{}/{}.bin", baseUrl, z, x, y);
    std::vector<uint8_t> raw;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &raw);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || httpCode != 200 || raw.size() < 32) {
        return std::nullopt;
    }

    uint32_t vertCount = 0;
    uint32_t idxCount = 0;
    double origin[3] = { 0.0, 0.0, 0.0 };

    std::memcpy(&vertCount, raw.data(), 4);
    std::memcpy(&idxCount, raw.data() + 4, 4);
    std::memcpy(origin, raw.data() + 8, 24);

    const size_t vertBytes = static_cast<size_t>(vertCount) * 8 * sizeof(float);
    const size_t idxBytes = static_cast<size_t>(idxCount) * sizeof(uint32_t);
    if (raw.size() < 32 + vertBytes + idxBytes) {
        return std::nullopt;
    }

    TileMeshData mesh;
    mesh.key = std::format("{}_{}_{}", z, x, y);
    mesh.originEcef = glm::dvec3(origin[0], origin[1], origin[2]);
    mesh.vertices.resize(vertCount * 8);
    mesh.indices.resize(idxCount);

    std::memcpy(mesh.vertices.data(), raw.data() + 32, vertBytes);
    std::memcpy(mesh.indices.data(), raw.data() + 32 + vertBytes, idxBytes);
    return mesh;
}

void RenderableOsmBuildings::requestTileIfNeeded(int z, int x, int y) {
    const std::string key = std::format("{}_{}_{}", z, x, y);
    if (_gpuTiles.contains(key) || _inFlight.contains(key)) {
        return;
    }
    _inFlight.insert(key);
    const std::string url = _serverUrl.value();
    _pendingFutures.emplace_back(
        key,
        std::async(std::launch::async, [url, z, x, y]() {
            return fetchTileBinary(url, z, x, y);
        })
    );
}

void RenderableOsmBuildings::update(const UpdateData&) {
    auto it = _pendingFutures.begin();
    while (it != _pendingFutures.end()) {
        if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const std::string key = it->first;
            _inFlight.erase(key);

            std::optional<TileMeshData> result = it->second.get();
            if (result.has_value() && !result->indices.empty()) {
                GpuTile gpu;
                gpu.indexCount = static_cast<GLsizei>(result->indices.size());
                gpu.originEcef = result->originEcef;

                glCreateVertexArrays(1, &gpu.vao);
                glCreateBuffers(1, &gpu.vbo);
                glCreateBuffers(1, &gpu.ibo);

                glNamedBufferData(
                    gpu.vbo,
                    result->vertices.size() * sizeof(float),
                    result->vertices.data(),
                    GL_STATIC_DRAW
                );
                glNamedBufferData(
                    gpu.ibo,
                    result->indices.size() * sizeof(uint32_t),
                    result->indices.data(),
                    GL_STATIC_DRAW
                );

                glVertexArrayVertexBuffer(gpu.vao, 0, gpu.vbo, 0, 8 * sizeof(float));
                glVertexArrayElementBuffer(gpu.vao, gpu.ibo);

                glEnableVertexArrayAttrib(gpu.vao, 0);
                glVertexArrayAttribFormat(gpu.vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
                glVertexArrayAttribBinding(gpu.vao, 0, 0);

                glEnableVertexArrayAttrib(gpu.vao, 1);
                glVertexArrayAttribFormat(gpu.vao, 1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
                glVertexArrayAttribBinding(gpu.vao, 1, 0);

                glEnableVertexArrayAttrib(gpu.vao, 2);
                glVertexArrayAttribFormat(gpu.vao, 2, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float));
                glVertexArrayAttribBinding(gpu.vao, 2, 0);

                _gpuTiles.emplace(result->key, gpu);
            }
            it = _pendingFutures.erase(it);
        }
        else {
            ++it;
        }
    }
}

void RenderableOsmBuildings::render(const RenderData& data, RendererTasks&) {
    if (_gpuTiles.empty() && _inFlight.size() > 16) {
        return;
    }

    const glm::dmat4 modelTransform = calcModelTransform(data);
    const glm::dmat4 worldToModel = glm::inverse(modelTransform);
    const glm::dvec3 camEcef = glm::dvec3(
        worldToModel * glm::dvec4(data.camera.position(), 1.0)
    );

    const double distFromCenter = glm::length(camEcef);
    const double approxAlt = distFromCenter - EarthRadius;
    if (approxAlt > static_cast<double>(_activationAltitude.value())) {
        return;
    }

    const double latDeg = glm::degrees(
        std::asin(glm::clamp(camEcef.z / distFromCenter, -1.0, 1.0))
    );
    const double lonDeg = glm::degrees(std::atan2(camEcef.y, camEcef.x));

    const auto [cx, cy] = latLonToTile(latDeg, lonDeg, TileZoom);
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            requestTileIfNeeded(TileZoom, cx + dx, cy + dy);
        }
    }

    if (_gpuTiles.empty() || !_program) {
        return;
    }

    _program->activate();
    _program->setUniform("opacity", _buildingOpacity.value());

    const glm::dmat4 viewMat = data.camera.combinedViewMatrix();
    const glm::mat4 projMat = data.camera.projectionMatrix();

    // Transform sun direction into camera/view space to match normalMatrix
    const glm::vec3 sunDirView = glm::normalize(
        glm::mat3(viewMat * modelTransform) * glm::vec3(1.0f, 0.5f, 0.5f)
    );
    _program->setUniform("sunDirection", sunDirView);

    for (const auto& [key, tile] : _gpuTiles) {
        const glm::dmat4 tileModel = glm::translate(modelTransform, tile.originEcef);
        const glm::mat4 modelView = glm::mat4(viewMat * tileModel);

        _program->setUniform("modelView", modelView);
        _program->setUniform("modelViewProjection", projMat * modelView);
        _program->setUniform("normalMatrix", glm::mat3(glm::transpose(glm::inverse(modelView))));

        glBindVertexArray(tile.vao);
        glDrawElements(GL_TRIANGLES, tile.indexCount, GL_UNSIGNED_INT, nullptr);
    }

    glBindVertexArray(0);
    _program->deactivate();
}

} // namespace openspace