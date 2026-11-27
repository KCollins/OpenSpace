#include <modules/osmbuildings/osmbuildingsmodule.h>

#include <modules/osmbuildings/rendering/renderableosmbuildings.h>
#include <openspace/documentation/documentation.h>
#include <openspace/misc/templatefactory.h>
#include <openspace/rendering/renderable.h>
#include <openspace/util/factorymanager.h>
#include <cassert>

namespace openspace {

OsmBuildingsModule::OsmBuildingsModule()
    : OpenSpaceModule(Name)
{}

void OsmBuildingsModule::internalInitialize(const Dictionary&) {
    TemplateFactory<Renderable>* fRenderable =
        FactoryManager::ref().factory<Renderable>();
    assert(fRenderable && "Renderable factory was not created");

    fRenderable->registerClass<RenderableOsmBuildings>("RenderableOsmBuildings");
}

} // namespace openspace