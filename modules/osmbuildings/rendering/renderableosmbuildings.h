#ifndef __OPENSPACE_MODULE_OSMBUILDINGS___RENDERABLEOSMBUILDINGS___H__
#define __OPENSPACE_MODULE_OSMBUILDINGS___RENDERABLEOSMBUILDINGS___H__

#include <openspace/rendering/renderable.h>

#include <openspace/properties/scalar/floatproperty.h>
#include <openspace/properties/misc/stringproperty.h>
#include <glm/glm.hpp>
#include <glbinding/gl/gl.h>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace openspace {

namespace opengl { class ProgramObject; }

class RenderableOsmBuildings : public Renderable {
public:
    explicit RenderableOsmBuildings(const Dictionary& dictionary);
    ~RenderableOsmBuildings() override = default;

    void initializeGL() override;
    void deinitializeGL() override;

    void render(const RenderData& data, RendererTasks& rendererTask) override;
    void update(const UpdateData& data) override;

    static openspace::Documentation Documentation();

private:
    struct TileMeshData {
        std::string key;
        glm::dvec3 originEcef = glm::dvec3(0.0);
        std::vector<float> vertices;
        std::vector<uint32_t> indices;
    };

    struct GpuTile {
        gl::GLuint vao = 0;
        gl::GLuint vbo = 0;
        gl::GLuint ibo = 0;
        gl::GLsizei indexCount = 0;
        glm::dvec3 originEcef = glm::dvec3(0.0);
    };

    void requestTileIfNeeded(int z, int x, int y);
    static std::optional<TileMeshData> fetchTileBinary(
        const std::string& baseUrl, int z, int x, int y
    );

    StringProperty _serverUrl;
    FloatProperty _activationAltitude;
    FloatProperty _buildingOpacity;

    std::unique_ptr<opengl::ProgramObject> _program;
    std::unordered_map<std::string, GpuTile> _gpuTiles;
    std::unordered_set<std::string> _inFlight;
    std::vector<std::pair<std::string, std::future<std::optional<TileMeshData>>>> _pendingFutures;
};

} // namespace openspace

#endif // __OPENSPACE_MODULE_OSMBUILDINGS___RENDERABLEOSMBUILDINGS___H__